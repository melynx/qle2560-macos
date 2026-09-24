// SPDX-License-Identifier: GPL-2.0-only
#include "QLE2560Driver.h"
#include "Controller.hpp"
#include "../protocol/Shutdown.hpp"
#include <DriverKit/IOTimerDispatchSource.h>
#include <DriverKit/IOInterruptDispatchSource.h>
#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/OSAction.h>
#include <DriverKit/OSDictionary.h>
#include <DriverKit/OSString.h>
#include <DriverKit/OSNumber.h>

struct QLE2560Driver_IVars {
    IOPCIDevice *pci = nullptr;
    Controller *controller = nullptr;
    IODispatchQueue *queue = nullptr, *auxiliary = nullptr;
    IOTimerDispatchSource *timer = nullptr;
    OSAction *timerAction = nullptr;
    IOInterruptDispatchSource *interrupt = nullptr;
    OSAction *interruptAction = nullptr;
    bool interruptSeen = false;
    tape::Shutdown shutdown;
#if QLE_SHUTDOWN_TESTING
    tape::ShutdownFault shutdownFault;
#endif
    bool offline = false, disconnected = false, powerPending = false;
    uint32_t powerFlags = 0;
    int flushSlot = -1;
    OSAction *disconnectAction = nullptr;
    SCSIUserParallelTask disconnectRequest{};
    bool cancellingSources = false, stopPrepared = false, stopCompleted = false;
    std::atomic<unsigned> pendingCancellations{0};
    IOService *stopProvider = nullptr;
    OSAction *actions[Controller::taskCount]{};
    SCSIUserParallelTask requests[Controller::taskCount]{};
    uint32_t mappedTasks = 0, recoveryAttempts = 0;
    uint64_t nextDiscovery = 0, lastRecovery = 0;
    bool opened = false, awake = true, running = false;
    std::atomic<bool> stopping{false}, publishing{false}, published{false}, removing{false};
    std::atomic<uint64_t> publishedWWPN{0};
};

bool QLE2560Driver::init() {
    ivars = nullptr;
    if (!super::init())
        return false;
    ivars = new QLE2560Driver_IVars;
    return ivars != nullptr;
}
void QLE2560Driver::free() {
    if (ivars) {
        if (ivars->controller)
            delete ivars->controller;
        if (ivars->timerAction)
            ivars->timerAction->release();
        if (ivars->timer)
            ivars->timer->release();
        if (ivars->interruptAction)
            ivars->interruptAction->release();
        if (ivars->interrupt)
            ivars->interrupt->release();
        if (ivars->auxiliary)
            ivars->auxiliary->release();
        if (ivars->queue)
            ivars->queue->release();
        if (ivars->pci)
            ivars->pci->release();
        delete ivars;
    }
    super::free();
}
kern_return_t IMPL(QLE2560Driver, Start) {
    os_log(OS_LOG_DEFAULT,
           "QLE2560: starting controller (version recorded in signed bundle metadata)");
    ivars->pci = OSDynamicCast(IOPCIDevice, provider);
    if (!ivars->pci)
        return kIOReturnUnsupported;
    ivars->pci->retain();
    return Start(provider, SUPERDISPATCH);
}
kern_return_t IMPL(QLE2560Driver, UserInitializeController) {
    os_log(OS_LOG_DEFAULT, "QLE2560: initializing SCSI controller");
    if (!ivars->pci)
        return kIOReturnNotReady;
    auto result = ivars->pci->Open(this, 0);
    if (result)
        return result;
    ivars->opened = true;
    uint32_t id = UINT32_MAX, sub = UINT32_MAX;
    ivars->pci->ConfigurationRead32(0, &id);
    ivars->pci->ConfigurationRead32(0x2c, &sub);
    uint8_t index = 0, type = 0;
    uint64_t size = 0;
    result = ivars->pci->GetBARInfo(1, &index, &size, &type);
    if (result || id != 0x25321077 || sub != 0x015c1077 ||
        (type != kPCIBARTypeM32 && type != kPCIBARTypeM64) || size < 0xc0) {
        ivars->pci->Close(this, 0);
        ivars->opened = false;
        return kIOReturnUnsupported;
    }
    result = CopyDispatchQueue(kIOServiceDefaultQueueName, &ivars->queue);
    if (!result)
        result = IODispatchQueue::Create("AuxiliaryQueue", 0, 0, &ivars->auxiliary);
    if (!result)
        result = SetDispatchQueue("AuxiliaryQueue", ivars->auxiliary);
    if (!result)
        result = IOTimerDispatchSource::Create(ivars->queue, &ivars->timer);
    if (!result)
        result = CreateActionTick(0, &ivars->timerAction);
    if (!result)
        result = ivars->timer->SetHandler(ivars->timerAction);
    if (!result) {
        auto irqResult =
            IOInterruptDispatchSource::Create(ivars->pci, 0, ivars->queue, &ivars->interrupt);
        if (!irqResult)
            irqResult = CreateActionInterrupt(0, &ivars->interruptAction);
        if (!irqResult)
            irqResult = ivars->interrupt->SetHandler(ivars->interruptAction);
        if (irqResult) {
            os_log(OS_LOG_DEFAULT, "QLE2560: interrupt setup unavailable=%x; using polling",
                   irqResult);
            if (ivars->interruptAction) {
                ivars->interruptAction->release();
                ivars->interruptAction = nullptr;
            }
            if (ivars->interrupt) {
                ivars->interrupt->release();
                ivars->interrupt = nullptr;
            }
        }
    }
    if (!result) {
        ivars->controller = new Controller(ivars->pci, index);
        if (!ivars->controller)
            result = kIOReturnNoMemory;
        else if (!ivars->controller->initialize())
            result = kIOReturnIOError;
    }
    if (result) {
        CancelSources();
        if (ivars->controller)
            ivars->controller->shutdown();
        ivars->pci->Close(this, 0);
        ivars->opened = false;
        if (ivars->controller)
            ivars->controller->shutdown(true);
        os_log(OS_LOG_DEFAULT, "QLE2560: initialization failed=%x", result);
    }
    return result;
}
kern_return_t IMPL(QLE2560Driver, UserStartController) {
    if (!ivars->controller || !ivars->controller->initialized)
        return kIOReturnNotReady;
    ivars->running = true;
    if (ivars->interrupt && !ivars->interrupt->SetEnable(true))
        ivars->controller->io.write32(0x0c, 8);
    auto result = ivars->timer->SetEnable(true);
    if (!result)
        Schedule();
    return result;
}
void QLE2560Driver::Schedule() {
    bool busy = false;
    if (ivars->controller)
        for (const auto &task : ivars->controller->tasks)
            busy |= task.active;
    if (!ivars->stopping && ivars->awake && ivars->running && !ivars->offline)
        ivars->timer->WakeAtTime(
            kIOTimerClockMonotonicRaw,
            clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) + (busy ? 2000000 : 250000000), 200000);
}
void QLE2560Driver::Complete(unsigned slot) {
    isp25xx::Completion completed;
    if (!ivars->controller->takeCompletion(slot, completed))
        return;
    if (ivars->shutdown.pending() && (completed.result != isp25xx::IOResult::complete ||
                                      completed.status != kSCSITaskStatus_GOOD))
        ivars->shutdown.step = tape::Shutdown::Step::failed;
    if (completed.result == isp25xx::IOResult::timeout)
        os_log(OS_LOG_DEFAULT, "QLE2560: timed-out task completed slot=%u; releasing request",
               slot);
    if (completed.result == isp25xx::IOResult::complete &&
        completed.status == kSCSITaskStatus_GOOD &&
        ivars->controller->io.nowMicros() - ivars->lastRecovery >= 60000000)
        ivars->recoveryAttempts = 0;
    auto *action = ivars->actions[slot];
    ivars->actions[slot] = nullptr;
    if (!action)
        return;
    SCSIUserParallelResponse response{};
    response.version = kScsiUserParallelTaskResponseCurrentVersion1;
    response.fTargetID = ivars->requests[slot].fTargetID;
    response.fControllerTaskIdentifier = ivars->requests[slot].fControllerTaskIdentifier;
    if (completed.result == isp25xx::IOResult::complete) {
        response.fServiceResponse = kSCSIServiceResponse_TASK_COMPLETE;
        response.fCompletionStatus = static_cast<SCSITaskStatus>(completed.status);
        response.fBytesTransferred = completed.transferred;
    } else {
        response.fServiceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
        response.fCompletionStatus =
            completed.result == isp25xx::IOResult::timeout    ? kSCSITaskStatus_TaskTimeoutOccurred
            : completed.result == isp25xx::IOResult::noDevice ? kSCSITaskStatus_DeviceNotPresent
                                                              : kSCSITaskStatus_DeliveryFailure;
    }
    response.fSenseLength = uint8_t(completed.senseLength);
    memcpy(response.fSenseBuffer, completed.sense, completed.senseLength);
    ParallelTaskCompletion(action, response);
    action->release();
}
bool QLE2560Driver::Quiesce(uint8_t status) {
    auto *core = ivars->controller;
    if (!core)
        return true;
    bool safe = core->shutdown(!ivars->opened);
    // Close disables bus mastering before framework-owned task DMA is released.
    if (!safe && ivars->opened) {
        ivars->pci->Close(this, 0);
        ivars->opened = false;
        safe = core->shutdown(true);
    }
    if (!safe)
        return false;
    for (unsigned i = 0; i < Controller::taskCount; ++i) {
        if (!ivars->actions[i])
            continue;
        auto *action = ivars->actions[i];
        ivars->actions[i] = nullptr;
        core->tasks[i].active = core->tasks[i].done = false;
        SCSIUserParallelResponse response{};
        response.version = 1;
        response.fTargetID = ivars->requests[i].fTargetID;
        response.fControllerTaskIdentifier = ivars->requests[i].fControllerTaskIdentifier;
        response.fServiceResponse = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
        response.fCompletionStatus =
            (core->tasks[i].timeoutAbort || core->io.nowMicros() >= core->tasks[i].deadline)
                ? kSCSITaskStatus_TaskTimeoutOccurred
                : static_cast<SCSITaskStatus>(status);
        ParallelTaskCompletion(action, response);
        action->release();
    }
    core->clearAfterQuiesce();
    core->targetReady = false;
    core->fatal = false;
    return true;
}
void QLE2560Driver::Publish(bool present) {
    if (!ivars->auxiliary || ivars->publishing.exchange(true))
        return;
    if (present == ivars->published.load()) {
        ivars->publishing = false;
        return;
    }
    ivars->removing = !present;
    const uint64_t wwpn = ivars->controller ? ivars->controller->targetWWPN : 0;
    // Target creation calls back on Default; run it on Auxiliary to avoid deadlock.
    retain();
    ivars->auxiliary->DispatchAsync(^{
      kern_return_t result = kIOReturnSuccess;
      if (present && !ivars->stopping) {
          auto *properties = OSDictionary::withCapacity(1);
          if (properties) {
              result = UserCreateTargetForID(0, properties);
              properties->release();
          } else
              result = kIOReturnNoMemory;
          if (!result) {
              ivars->published = true;
              ivars->publishedWWPN = wwpn;
              if (ivars->stopping && !UserDestroyTargetForID(0))
                  ivars->published = false;
          }
      } else if (ivars->published) {
          result = UserDestroyTargetForID(0);
          if (!result)
              ivars->published = false;
      }
      os_log(OS_LOG_DEFAULT, "QLE2560: target publication present=%u result=%x", present, result);
      ivars->removing = false;
      ivars->publishing = false;
      retain();
      ivars->queue->DispatchAsync(^{
        FinishStop();
        release();
      });
      release();
    });
}
void IMPL(QLE2560Driver, Tick) {
    Service();
}
void IMPL(QLE2560Driver, Interrupt) {
    if (!ivars->interruptSeen) {
        ivars->interruptSeen = true;
        os_log(OS_LOG_DEFAULT, "QLE2560: hardware interrupt received count=%llu", count);
    }
    Service();
}
void QLE2560Driver::Service() {
    if (ivars->stopping || !ivars->awake || !ivars->running || ivars->offline)
        return;
    auto *core = ivars->controller;
    core->poll();
    for (unsigned i = 0; i < Controller::taskCount; ++i)
        if (int(i) != ivars->flushSlot)
            Complete(i);
    if (ivars->shutdown.pending() || ivars->powerPending || ivars->disconnectAction) {
        AdvanceShutdown();
        Schedule();
        return;
    }
    if (core->fatal) {
        os_log(OS_LOG_DEFAULT,
               "QLE2560: controller fault; quiescing outstanding I/O without replay");
        if (!Quiesce(kSCSITaskStatus_DeliveryFailure)) {
            ivars->running = false;
            return;
        }
        Publish(false);
        ivars->lastRecovery = core->io.nowMicros();
        if (++ivars->recoveryAttempts >= 3) {
            ivars->running = false;
            os_log(OS_LOG_DEFAULT,
                   "QLE2560: repeated controller faults; stopped after three recoveries");
            return;
        }
        ivars->nextDiscovery = core->io.nowMicros() + 5000000;
    }
    const auto now = core->io.nowMicros();
    if (now >= ivars->nextDiscovery && !ivars->publishing) {
        ivars->nextDiscovery = now + 2000000;
        if (!core->initialized) {
            if (!ivars->opened) {
                const auto result = ivars->pci->Open(this, 0);
                os_log(OS_LOG_DEFAULT, "QLE2560: reopening PCI session result=%x", result);
                if (result) {
                    Schedule();
                    return;
                }
                ivars->opened = true;
            }
            // Close disables PCI memory decoding as well as bus mastering.
            // Restore register access with DMA still disabled before any MMIO.
            if (!core->io.enableDMA(false) || core->io.read32(8) == UINT32_MAX) {
                os_log(OS_LOG_DEFAULT, "QLE2560: PCI register access unavailable after reopen");
                ivars->pci->Close(this, 0);
                ivars->opened = false;
                ivars->running = false;
                return;
            }
            if (!core->initialize()) {
                Quiesce(kSCSITaskStatus_DeviceNotResponding);
                ivars->lastRecovery = core->io.nowMicros();
                if (++ivars->recoveryAttempts >= 3)
                    ivars->running = false;
                Schedule();
                return;
            }
            if (ivars->interrupt && !ivars->interrupt->SetEnable(true))
                core->io.write32(0x0c, 8);
        }
        // Do not run discovery mailboxes while commands are outstanding.
        bool busy = false;
        for (auto &task : core->tasks)
            busy |= task.active;
        if (!busy && core->queryLink()) {
            if (core->firmwareState == 3) {
                if (!core->targetReady || core->linkChanged)
                    core->discover();
                if (ivars->published && ivars->publishedWWPN != core->targetWWPN)
                    Publish(false);
                else
                    Publish(core->targetReady);
            } else {
                core->targetReady = false;
                Publish(false);
            }
            core->linkChanged = false;
        }
    }
    Schedule();
}
void QLE2560Driver::CancelSources() {
    if (ivars->cancellingSources)
        return;
    ivars->cancellingSources = true;
    // Establish the complete count before Cancel: callbacks may run promptly.
    ivars->pendingCancellations = (ivars->timer ? 1u : 0u) + (ivars->interrupt ? 1u : 0u);
    if (ivars->timer) {
        retain();
        ivars->timer->Cancel(^{
          auto *timer = ivars->timer;
          ivars->timer = nullptr;
          auto *action = ivars->timerAction;
          ivars->timerAction = nullptr;
          if (action)
              action->release();
          if (timer)
              timer->release();
          --ivars->pendingCancellations;
          retain();
          ivars->queue->DispatchAsync(^{
            FinishStop();
            release();
          });
          release();
        });
    }
    if (ivars->interrupt) {
        retain();
        ivars->interrupt->Cancel(^{
          auto *source = ivars->interrupt;
          ivars->interrupt = nullptr;
          auto *action = ivars->interruptAction;
          ivars->interruptAction = nullptr;
          if (action)
              action->release();
          if (source)
              source->release();
          --ivars->pendingCancellations;
          retain();
          ivars->queue->DispatchAsync(^{
            FinishStop();
            release();
          });
          release();
        });
    }
}
void QLE2560Driver::FinishStop() {
    // Runs on Default. Never block that queue waiting for cancellation callbacks.
    if (!ivars->stopPrepared || ivars->stopCompleted || ivars->pendingCancellations.load() ||
        ivars->publishing.load())
        return;
    ivars->stopCompleted = true;
    auto *provider = ivars->stopProvider;
    ivars->stopProvider = nullptr;
    os_log(OS_LOG_DEFAULT, "QLE2560: dispatch cancellation complete; calling superclass Stop");
    const auto result = Stop(provider, SUPERDISPATCH);
    provider->release();
    os_log(OS_LOG_DEFAULT, "QLE2560: superclass Stop completed result=%x", result);
    release(); // Retain taken by Stop for the asynchronous completion.
}
kern_return_t IMPL(QLE2560Driver, Stop) {
    if (ivars->stopProvider || ivars->stopCompleted)
        return kIOReturnSuccess;
    retain();
    provider->retain();
    ivars->stopProvider = provider;
    ivars->stopping = true;
    ivars->running = false;
    if (ivars->powerPending || ivars->disconnectAction)
        FinishShutdown(false);
    os_log(OS_LOG_DEFAULT, "QLE2560: stopping controller; draining dispatch sources");
    CancelSources();
    Quiesce(kSCSITaskStatus_DeviceNotPresent);
    if (ivars->opened) {
        ivars->pci->Close(this, 0);
        ivars->opened = false;
    }
    ivars->stopPrepared = true;
    // An in-flight target publication owns its own retain and wakes FinishStop.
    FinishStop();
    return kIOReturnSuccess;
}
// The superclass call acknowledges the power transition. Defer it while the
// Default queue continues servicing interrupts/timers. Apple's kernel requests
// a 20-second acknowledgement; use 10 seconds here, leaving reset/close margin.
void QLE2560Driver::AdvanceShutdown() {
    auto *core = ivars->controller;
    auto &state = ivars->shutdown;
    if (core->fatal || state.expired(core->io.nowMicros())) {
        os_log(OS_LOG_DEFAULT, "QLE2560: shutdown failed fatal=%u deadline=%llu now=%llu",
               core->fatal, state.deadline, core->io.nowMicros());
        FinishShutdown(false);
        return;
    }
    if (ivars->flushSlot >= 0) {
#if QLE_SHUTDOWN_TESTING
        // Continue polling hardware normally, but withhold consumption of the
        // internal flush result until the real shutdown deadline expires.
        // Never change DMA ownership, fabricate success, or shorten the budget.
        if (ivars->shutdownFault.active)
            return;
#endif
        isp25xx::Completion done;
        if (!core->takeCompletion(unsigned(ivars->flushSlot), done))
            return;
        ivars->flushSlot = -1;
        const bool good =
            done.result == isp25xx::IOResult::complete && done.status == kSCSITaskStatus_GOOD;
        os_log(OS_LOG_DEFAULT, "QLE2560: shutdown flush lun=%u good=%u status=%x senseLength=%u",
               state.cursor, good, done.status, done.senseLength);
        state.flushed(good);
        if (!good) {
            FinishShutdown(false);
            return;
        }
    }
    bool busy = false;
    for (const auto &task : core->tasks)
        busy |= task.active;
    const int lun = state.nextFlush(busy);
    if (lun >= 0) {
        // All host tasks have drained, so slot zero is available. No data DMA.
        uint8_t cdb[6] = {0x10, 0, 0, 0, 0, 0};
        if (!core->submit(0, state.luns[lun], cdb, 6, 0, 0, 0, 0, 0)) {
            FinishShutdown(false);
            return;
        }
        ivars->flushSlot = 0;
#if QLE_SHUTDOWN_TESTING
        if (ivars->shutdownFault.active)
            os_log(OS_LOG_DEFAULT,
                   "QLE2560: TEST withholding shutdown flush result until deadline");
#endif
        os_log(OS_LOG_DEFAULT, "QLE2560: shutdown flushing lun=%u", unsigned(lun));
    }
    if (state.step == tape::Shutdown::Step::finished)
        FinishShutdown(true);
}
void QLE2560Driver::FinishShutdown(bool clean) {
#if QLE_SHUTDOWN_TESTING
    ivars->shutdownFault.finish();
#endif
    // Block further device access before tearing down hardware. Keep the target
    // published so the exclusive SCSI client can receive the final control reply.
    ivars->offline = true;
    const bool safe = Quiesce(kSCSITaskStatus_DeliveryFailure);
    if (ivars->opened) {
        ivars->pci->Close(this, 0);
        ivars->opened = false;
    }
    ivars->flushSlot = -1;
    clean = clean && safe;
    ivars->shutdown.step = clean ? tape::Shutdown::Step::finished : tape::Shutdown::Step::failed;
    if (ivars->interrupt)
        ivars->interrupt->SetEnable(false);
    if (ivars->timer)
        ivars->timer->SetEnable(false);
    os_log(OS_LOG_DEFAULT, "QLE2560: coordinated shutdown clean=%u power=%u", clean,
           ivars->powerPending);
    if (ivars->disconnectAction) {
        SCSIUserParallelResponse reply{};
        reply.version = 1;
        reply.fTargetID = ivars->disconnectRequest.fTargetID;
        reply.fControllerTaskIdentifier = ivars->disconnectRequest.fControllerTaskIdentifier;
        reply.fServiceResponse = clean ? kSCSIServiceResponse_TASK_COMPLETE
                                       : kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
        reply.fCompletionStatus = clean ? kSCSITaskStatus_GOOD : kSCSITaskStatus_DeliveryFailure;
        auto *action = ivars->disconnectAction;
        ivars->disconnectAction = nullptr;
        ParallelTaskCompletion(action, reply);
        action->release();
    }
    if (ivars->powerPending) {
        const uint32_t flags = ivars->powerFlags;
        ivars->powerPending = false;
        ivars->awake = false;
        // A normal sleep resumes on wake; explicit safe-disconnect stays offline.
        ivars->offline = ivars->disconnected;
        const auto result = SetPowerState(flags, SUPERDISPATCH);
        os_log(OS_LOG_DEFAULT, "QLE2560: power shutdown acknowledged result=%x", result);
    }
}
kern_return_t IMPL(QLE2560Driver, SetPowerState) {
    const bool on = (powerFlags & kIOServicePowerCapabilityOn) != 0;
    if (!on && ivars->awake && ivars->running && !ivars->offline) {
        ivars->powerPending = true;
        ivars->powerFlags = powerFlags;
        const uint64_t now = ivars->controller->io.nowMicros();
        const bool began = ivars->shutdown.begin(now, 10000000);
#if QLE_SHUTDOWN_TESTING
        if (began)
            ivars->shutdownFault.begin();
#endif
        if (!began && ivars->shutdown.deadline > now + 10000000)
            ivars->shutdown.deadline = now + 10000000;
        os_log(OS_LOG_DEFAULT, "QLE2560: power shutdown draining requests");
        Schedule();
        return kIOReturnSuccess;
    }
    if (!on)
        ivars->awake = false;
    else if (!ivars->awake) {
        ivars->awake = true;
        ivars->nextDiscovery = 0;
        if (!ivars->offline && ivars->timer)
            ivars->timer->SetEnable(true);
        Schedule();
    }
    return SetPowerState(powerFlags, SUPERDISPATCH);
}
void QLE2560Driver::CompleteLocal(SCSIUserParallelTask request, OSAction *action, uint8_t status) {
    SCSIUserParallelResponse reply{};
    reply.version = 1;
    reply.fTargetID = request.fTargetID;
    reply.fControllerTaskIdentifier = request.fControllerTaskIdentifier;
    reply.fServiceResponse = status == kSCSITaskStatus_GOOD
                                 ? kSCSIServiceResponse_TASK_COMPLETE
                                 : kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    reply.fCompletionStatus = static_cast<SCSITaskStatus>(status);
    // Run after UserProcessParallelTask returns Request_In_Process. This also
    // avoids racing an inline completion against framework request registration.
    retain();
    action->retain();
    ivars->queue->DispatchAsync(^{
      ParallelTaskCompletion(action, reply);
      action->release();
      release();
    });
}
kern_return_t IMPL(QLE2560Driver, UserProcessParallelTask) {
    *response = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    if (!completion || !ivars->queue)
        return kIOReturnBadArgument;
    // The framework has already prepared the request's DMA mapping. Even a
    // request we never submit to firmware must use its completion action.
    auto reject = [&]() {
        *response = kSCSIServiceResponse_Request_In_Process;
        CompleteLocal(parallelRequest, completion, kSCSITaskStatus_DeliveryFailure);
        return kIOReturnSuccess;
    };
    if (ivars->stopping || ivars->removing || !ivars->awake || !ivars->running ||
        parallelRequest.version != 1 || parallelRequest.fTargetID != 0 ||
        parallelRequest.fControllerTaskIdentifier >= Controller::taskCount ||
        parallelRequest.fRequestedTransferCount > 8 * 1024 * 1024 ||
        parallelRequest.fTaskAttribute > 3 || parallelRequest.fTransferDirection > 2)
        return reject();
    const unsigned slot = unsigned(parallelRequest.fControllerTaskIdentifier);
    if (ivars->actions[slot] || ivars->controller->tasks[slot].active)
        return reject();
    const auto *lun = parallelRequest.fLogicalUnitBytes;
    if ((lun[0] != 0 && lun[0] != 0x40) || lun[2] || lun[3] || lun[4] || lun[5] || lun[6] || lun[7])
        return reject();
    const uint8_t operation = tape::controlOperation(parallelRequest.fCommandDescriptorBlock,
                                                     parallelRequest.fCommandSize);
    if (operation) {
        if (parallelRequest.fRequestedTransferCount || parallelRequest.fTransferDirection)
            return reject();
        if (ivars->powerPending || ivars->disconnectAction)
            return reject();
        if (operation >= 3) {
#if QLE_SHUTDOWN_TESTING
            if (ivars->offline || ivars->shutdown.pending())
                return reject();
            ivars->shutdownFault.armed = operation == 3;
            if (operation == 3)
                ivars->shutdown.observeWrite(lun);
            os_log(OS_LOG_DEFAULT, "QLE2560: TEST shutdown deadline armed=%u",
                   ivars->shutdownFault.armed);
            *response = kSCSIServiceResponse_Request_In_Process;
            CompleteLocal(parallelRequest, completion, kSCSITaskStatus_GOOD);
            return kIOReturnSuccess;
#else
            return reject();
#endif
        }
        if (operation == 1) {
            if (ivars->offline)
                return reject();
            ivars->disconnected = true;
            // Flush the selected tape even if this driver instance has not
            // observed its writes (for example, after a driver upgrade).
            ivars->shutdown.observeWrite(lun);
            ivars->disconnectRequest = parallelRequest;
            completion->retain();
            ivars->disconnectAction = completion;
            ivars->shutdown.begin(ivars->controller->io.nowMicros(), 60000000);
#if QLE_SHUTDOWN_TESTING
            ivars->shutdownFault.begin();
#endif
            *response = kSCSIServiceResponse_Request_In_Process;
            Schedule();
            return kIOReturnSuccess;
        }
        if (!ivars->offline || !ivars->disconnected)
            return reject();
        ivars->offline = false;
        ivars->disconnected = false;
        ivars->nextDiscovery = 0;
        ivars->shutdown.step = tape::Shutdown::Step::idle;
        const auto timerResult = ivars->timer ? ivars->timer->SetEnable(true) : kIOReturnNotReady;
        os_log(OS_LOG_DEFAULT, "QLE2560: software reconnect requested timer=%x", timerResult);
        if (timerResult) {
            ivars->offline = true;
            ivars->disconnected = true;
            return reject();
        }
        *response = kSCSIServiceResponse_Request_In_Process;
        CompleteLocal(parallelRequest, completion, kSCSITaskStatus_GOOD);
        Schedule();
        return kIOReturnSuccess;
    }
    if (ivars->offline || ivars->shutdown.pending() || ivars->powerPending ||
        ivars->disconnectAction)
        return reject();
    const uint8_t attributes[] = {0, 2, 1, 4};
    const uint8_t direction = parallelRequest.fTransferDirection == 2   ? 1
                              : parallelRequest.fTransferDirection == 1 ? 2
                                                                        : 0;
    if (!ivars->controller->submit(
            slot, parallelRequest.fLogicalUnitBytes, parallelRequest.fCommandDescriptorBlock,
            parallelRequest.fCommandSize, parallelRequest.fBufferIOVMAddr,
            uint32_t(parallelRequest.fRequestedTransferCount), direction,
            attributes[parallelRequest.fTaskAttribute], parallelRequest.fTimeoutInMilliSec))
        return reject();
    const uint8_t opcode = parallelRequest.fCommandDescriptorBlock[0];
    if (opcode == 0x0a || opcode == 0x8a || opcode == 0x10 || opcode == 0x80)
        ivars->shutdown.observeWrite(lun);
    ivars->requests[slot] = parallelRequest;
    completion->retain();
    ivars->actions[slot] = completion;
    *response = kSCSIServiceResponse_Request_In_Process;
    Schedule();
    return kIOReturnSuccess;
}
kern_return_t IMPL(QLE2560Driver, UserReportHBAHighestLogicalUnitNumber) {
    *value = 255;
    return kIOReturnSuccess;
}
kern_return_t IMPL(QLE2560Driver, UserDoesHBASupportSCSIParallelFeature) {
    *result = false;
    return kIOReturnSuccess;
}
kern_return_t IMPL(QLE2560Driver, UserInitializeTargetForID) {
    return targetID == 0 && ivars->controller && ivars->controller->targetReady ? kIOReturnSuccess
                                                                                : kIOReturnNoDevice;
}
kern_return_t IMPL(QLE2560Driver, UserDoesHBAPerformAutoSense) {
    *result = true;
    return kIOReturnSuccess;
}
kern_return_t IMPL(QLE2560Driver, UserDoesHBASupportMultiPathing) {
    *result = false;
    return kIOReturnSuccess;
}
kern_return_t IMPL(QLE2560Driver, UserReportInitiatorIdentifier) {
    *id = 1;
    return kIOReturnSuccess;
}
kern_return_t IMPL(QLE2560Driver, UserReportHighestSupportedDeviceID) {
    *id = 1;
    return kIOReturnSuccess;
}
kern_return_t IMPL(QLE2560Driver, UserReportMaximumTaskCount) {
    *count = Controller::taskCount;
    return kIOReturnSuccess;
}
kern_return_t IMPL(QLE2560Driver, UserDoesHBAPerformDeviceManagement) {
    *result = true;
    return kIOReturnSuccess;
}
kern_return_t IMPL(QLE2560Driver, UserGetDMASpecification) {
    *maxTransferSize = 8 * 1024 * 1024;
    *alignment = 1;
    *numAddressBits = 64;
    *segmentType = kDMAOutputSegmentHost64;
    return kIOReturnSuccess;
}
kern_return_t IMPL(QLE2560Driver, UserMapHBAData) {
    if (ivars->mappedTasks >= Controller::taskCount)
        return kIOReturnNoResources;
    *uniqueTaskID = ivars->mappedTasks++;
    return kIOReturnSuccess;
}
kern_return_t IMPL(QLE2560Driver, UserMapBundledParallelTaskCommandAndResponseBuffers) {
    return kIOReturnUnsupported;
}
void IMPL(QLE2560Driver, UserProcessBundledParallelTasks) {
    // The framework must use the unbundled path after mapping was declined.
    os_log(OS_LOG_DEFAULT, "QLE2560: unexpected bundled task delivery");
}
kern_return_t QLE2560Driver::Manage(uint64_t target, uint64_t lun, uint32_t flags,
                                    uint32_t *response) {
    *response = kSCSIServiceResponse_FUNCTION_REJECTED;
    if (target != 0 || !ivars->controller || !ivars->awake || ivars->stopping || ivars->offline ||
        ivars->shutdown.pending())
        return kIOReturnSuccess;
    if (ivars->controller->taskManagement(lun, flags))
        *response = kSCSIServiceResponse_FUNCTION_COMPLETE;
    return kIOReturnSuccess;
}
kern_return_t IMPL(QLE2560Driver, UserAbortTaskRequest) {
    *response = kSCSIServiceResponse_FUNCTION_REJECTED;
    if (theT != 0 || !ivars->controller || ivars->offline || ivars->shutdown.pending())
        return kIOReturnSuccess;
    for (unsigned i = 0; i < Controller::taskCount; ++i) {
        const auto &request = ivars->requests[i];
        const uint64_t lun =
            uint64_t(request.fLogicalUnitBytes[0] & 0x3f) << 8 | request.fLogicalUnitBytes[1];
        if (ivars->actions[i] && request.fTaskTagIdentifier == theQ && lun == theL) {
            if (ivars->controller->abort(i))
                *response = kSCSIServiceResponse_FUNCTION_COMPLETE;
            return kIOReturnSuccess;
        }
    }
    *response = kSCSIServiceResponse_FUNCTION_COMPLETE;
    return kIOReturnSuccess;
}
kern_return_t IMPL(QLE2560Driver, UserAbortTaskSetRequest) {
    return Manage(theT, theL, 8, response);
}
kern_return_t IMPL(QLE2560Driver, UserClearACARequest) {
    return Manage(theT, theL, 1, response);
}
kern_return_t IMPL(QLE2560Driver, UserClearTaskSetRequest) {
    return Manage(theT, theL, 4, response);
}
kern_return_t IMPL(QLE2560Driver, UserLogicalUnitResetRequest) {
    return Manage(theT, theL, 16, response);
}
kern_return_t IMPL(QLE2560Driver, UserTargetResetRequest) {
    return Manage(theT, 0, 2, response);
}
