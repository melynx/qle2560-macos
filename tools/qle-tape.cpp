// SPDX-License-Identifier: GPL-2.0-only
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <IOKit/scsi/SCSITaskLib.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include "../protocol/Shutdown.hpp"
#include "../protocol/Tape.hpp"
#include "../protocol/TapeDiagnostics.hpp"
#include <chrono>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <cinttypes>
#include <vector>
#include <string>
#include <unistd.h>
#include <signal.h>

struct Reply {
    IOReturn error = kIOReturnError;
    SCSITaskStatus status = kSCSITaskStatus_No_Status;
    SCSIServiceResponse service = kSCSIServiceResponse_SERVICE_DELIVERY_OR_TARGET_FAILURE;
    uint64_t transferred = 0;
    uint8_t sense[252]{};
    tape::Sense parsed;
    bool good() const {
        return !error && service == kSCSIServiceResponse_TASK_COMPLETE &&
               status == kSCSITaskStatus_GOOD;
    }
};
struct IdleSleepAssertion {
    IOPMAssertionID id = kIOPMNullAssertionID;
    bool acquire() {
        return IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleSystemSleep,
                                           kIOPMAssertionLevelOn, CFSTR("QLE2560 tape operation"),
                                           &id) == kIOReturnSuccess;
    }
    ~IdleSleepAssertion() {
        if (id != kIOPMNullAssertionID)
            IOPMAssertionRelease(id);
    }
};
struct Device {
    SCSITaskDeviceInterface **interface = nullptr;
    uint32_t timeoutOverride = 0;
    ~Device() {
        if (interface) {
            (*interface)->ReleaseExclusiveAccess(interface);
            (*interface)->Release(interface);
        }
    }
    bool open(io_service_t service) {
        IOCFPlugInInterface **plugin = nullptr;
        SInt32 score = 0;
        auto r = IOCreatePlugInInterfaceForService(service, kIOSCSITaskDeviceUserClientTypeID,
                                                   kIOCFPlugInInterfaceID, &plugin, &score);
        if (r || !plugin)
            return false;
        HRESULT query =
            (*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOSCSITaskDeviceInterfaceID),
                                      reinterpret_cast<LPVOID *>(&interface));
        // QueryInterface retains the device interface. Drop only our plugin
        // reference; IODestroyPlugInInterface also stops the shared plugin.
        (*plugin)->Release(plugin);
        if (query || !interface)
            return false;
        r = (*interface)->ObtainExclusiveAccess(interface);
        if (r) {
            fprintf(stderr, "Exclusive tape access failed: 0x%x\n", r);
            (*interface)->Release(interface);
            interface = nullptr;
            return false;
        }
        return true;
    }
    Reply command(const uint8_t *cdb, uint8_t length, void *data = nullptr, uint32_t count = 0,
                  bool write = false, uint32_t timeout = 60000) {
        Reply result;
        auto **task = (*interface)->CreateSCSITask(interface);
        if (!task) {
            fprintf(stderr, "CreateSCSITask failed\n");
            return result;
        }
        const char *stage = "SetCommandDescriptorBlock";
        uint8_t local[16]{};
        memcpy(local, cdb, length);
        auto r = (*task)->SetCommandDescriptorBlock(task, local, length);
        if (!r) {
            stage = "SetTimeoutDuration";
            r = (*task)->SetTimeoutDuration(task, timeoutOverride ? timeoutOverride : timeout);
        }
        if (!r) {
            stage = "SetTaskAttribute";
            r = (*task)->SetTaskAttribute(task, kSCSITask_SIMPLE);
        }
        SCSITaskSGElement sg{};
        sg.address = reinterpret_cast<mach_vm_address_t>(data);
        sg.length = count;
        if (!r) {
            stage = "SetScatterGatherEntries";
            r = (*task)->SetScatterGatherEntries(
                task, count ? &sg : nullptr, count ? 1 : 0, count,
                count ? (write ? kSCSIDataTransfer_FromInitiatorToTarget
                               : kSCSIDataTransfer_FromTargetToInitiator)
                      : kSCSIDataTransfer_NoDataTransfer);
        }
        if (!r) {
            stage = "SetAutoSenseDataBuffer";
            r = (*task)->SetAutoSenseDataBuffer(
                task, reinterpret_cast<SCSI_Sense_Data *>(result.sense), sizeof(result.sense));
        }
        UInt64 transferred = 0;
        if (!r) {
            stage = "ExecuteTaskSync";
            r = (*task)->ExecuteTaskSync(task, nullptr, &result.status, &transferred);
        }
        if (r)
            fprintf(stderr, "%s failed: 0x%x\n", stage, r);
        result.error = r;
        result.transferred = transferred;
        (*task)->GetSCSIServiceResponse(task, &result.service);
        result.parsed = tape::decodeSense(result.sense, sizeof(result.sense));
        (*task)->Release(task);
        return result;
    }
};
static void report(const Reply &r) {
    fprintf(stderr,
            "SCSI error=0x%x service=%u status=0x%02x transferred=%" PRIu64
            " sense=%x/%02x/%02x filemark=%u EOM=%u ILI=%u\n",
            r.error, unsigned(r.service), unsigned(r.status), r.transferred, r.parsed.key,
            r.parsed.asc, r.parsed.ascq, r.parsed.filemark, r.parsed.eom, r.parsed.ili);
    if (r.parsed.valid) {
        // This is the sense header's declared length, bounded by our zero-filled
        // autosense buffer, not an independently reported received byte count.
        const size_t length = std::min(sizeof(r.sense), size_t(8) + r.sense[7]);
        fprintf(stderr,
                "Sense response=0x%02x deferred=%u information-valid=%u information=%" PRId64
                " raw-header-length=%zu raw=",
                r.sense[0] & 0x7f, r.parsed.deferred, r.parsed.informationValid,
                r.parsed.information, length);
        for (size_t i = 0; i < length; ++i)
            fprintf(stderr, "%s%02x", i ? " " : "", r.sense[i]);
        fputc('\n', stderr);
    }
}
static std::vector<io_service_t> devices() {
    auto matching = IOServiceMatching("IOService");
    auto properties = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(properties, CFSTR(kIOPropertySCSITaskDeviceCategory),
                         CFSTR(kIOPropertySCSITaskUserClientDevice));
    CFDictionarySetValue(matching, CFSTR(kIOPropertyMatchKey), properties);
    CFRelease(properties);
    io_iterator_t iterator = 0;
    std::vector<io_service_t> found;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iterator))
        return found;
    while (auto service = IOIteratorNext(iterator)) {
        CFTypeRef userClass = IORegistryEntrySearchCFProperty(
            service, kIOServicePlane, CFSTR("IOUserClass"), nullptr,
            kIORegistryIterateRecursively | kIORegistryIterateParents);
        bool ours = userClass && CFEqual(userClass, CFSTR("QLE2560Driver"));
        if (userClass)
            CFRelease(userClass);
        if (ours)
            found.push_back(service);
        else
            IOObjectRelease(service);
    }
    IOObjectRelease(iterator);
    return found;
}
static void usage() {
    fprintf(stderr,
            "Usage: qle-tape [--device registry-id] [--block-size bytes] [--timeout-ms "
            "milliseconds] command [count|object|page]\n"
            "Commands: list, inquiry, status, rewind, read, write, filemark, fsf, bsf, fsr, bsr, "
            "eod, unload, position, seek, health, alerts, log-page, limits, flush, "
            "safe-disconnect, reconnect\n"
            "read/write use stdin/stdout and variable tape records; write appends one filemark on "
            "success.\n"
            "seek OBJECT uses logical objects (records plus filemarks) in the current partition.\n"
            "--timeout-ms overrides command deadlines (1..1800000); timed-out commands are not "
            "retried.\n"
            "health/alerts read and clear latched TapeAlert flags. Exit 3 means active alerts.\n"
            "No automatic rewind on close. Position explicitly before writing. No automatic write "
            "retries.\n"
            "write exits 4 for a current end-of-medium CHECK CONDITION; final data/filemark may be "
            "incomplete.\n"
            "Examples: tar -cf - folder | qle-tape write\n"
            "          qle-tape rewind; qle-tape read > restored.tar\n");
}
static bool parseUnsigned(const char *p, uint64_t &out) {
    if (!p || !*p || *p == '-')
        return false;
    errno = 0;
    char *end = nullptr;
    out = strtoull(p, &end, 0);
    return !errno && end && !*end;
}
static bool outputAll(const uint8_t *data, size_t size) {
    while (size) {
        ssize_t n = ::write(STDOUT_FILENO, data, size);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        data += n;
        size -= size_t(n);
    }
    return true;
}
static bool ready(Device &device) {
    uint8_t cdb[6]{};
    for (unsigned i = 0; i < 4; ++i) {
        auto r = device.command(cdb, 6);
        if (r.good())
            return true;
        // Unit attention is consumed only before a new stream, never after an ambiguous write.
        if (r.error || r.service != kSCSIServiceResponse_TASK_COMPLETE || !r.parsed.valid ||
            r.parsed.key != 6) {
            report(r);
            return false;
        }
    }
    return false;
}
static bool variableMode(Device &device, uint32_t block) {
    uint8_t limits[6]{}, limitCDB[6] = {5};
    auto r = device.command(limitCDB, 6, limits, 6);
    if (!r.good() || r.transferred < 6) {
        report(r);
        return false;
    }
    const uint32_t max = uint32_t(limits[1]) << 16 | uint32_t(limits[2]) << 8 | limits[3];
    const uint16_t min = uint16_t(limits[4]) << 8 | limits[5];
    if ((max && block > max) || block < min) {
        fprintf(stderr, "Block size outside drive limits (%u..%u)\n", min, max);
        return false;
    }
    uint8_t mode[12]{}, senseCDB[6] = {0x1a, 0, 0, 0, 12, 0};
    r = device.command(senseCDB, 6, mode, 12);
    if (!r.good() || r.transferred < 12 || mode[3] < 8) {
        report(r);
        return false;
    }
    if (mode[9] == 0 && mode[10] == 0 && mode[11] == 0)
        return true;
    uint8_t parameters[12]{};
    parameters[2] = mode[2] & 0x70;
    parameters[3] = 8;
    parameters[4] = mode[4];
    uint8_t select[6] = {0x15, 0x10, 0, 0, 12, 0};
    r = device.command(select, 6, parameters, 12, true);
    if (!r.good()) {
        report(r);
        return false;
    }
    return true;
}
static bool position(Device &device, tape::Position &p) {
    uint8_t cdb[10] = {0x34, 6}, data[32]{}; // Long form requires zero allocation-length CDB field.
    const auto r = device.command(cdb, 10, data, sizeof(data), false, 180000);
    if (!r.good()) {
        report(r);
        return false;
    }
    if (!tape::decodePosition(data, size_t(r.transferred), p)) {
        fprintf(stderr, "Truncated READ POSITION response\n");
        return false;
    }
    printf("partition=%u object=", p.partition);
    if (p.objectKnown)
        printf("%" PRIu64, p.object);
    else
        printf("unknown");
    printf(" file=");
    if (p.fileKnown)
        printf("%" PRIu64, p.file);
    else
        printf("unknown");
    printf(" BOP=%u EOP=%u\n", p.bop, p.eop);
    return true;
}
static bool logPage(Device &device, uint8_t page, std::vector<uint8_t> &data) {
    // One full read: do not consume TapeAlert with a preliminary length query.
    uint8_t cdb[10] = {0x4d, 0, uint8_t(0x40 | page), 0, 0, 0, 0, 0xff, 0xff, 0};
    data.assign(65535, 0);
    Reply r;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        r = device.command(cdb, 10, data.data(), uint32_t(data.size()));
        if (r.good() || r.error || r.service != kSCSIServiceResponse_TASK_COMPLETE ||
            r.status != kSCSITaskStatus_CHECK_CONDITION || !r.parsed.valid || r.parsed.deferred ||
            r.parsed.key != 6 || r.transferred)
            break;
    }
    if (!r.good()) {
        report(r);
        return false;
    }
    size_t end = 0;
    if (r.transferred > data.size() ||
        !tape::logLength(data.data(), size_t(r.transferred), page, end)) {
        fprintf(stderr, "Invalid/truncated LOG SENSE page 0x%02x\n", page);
        return false;
    }
    data.resize(end);
    return true;
}
static int showLog(Device &device, uint8_t page) {
    std::vector<uint8_t> data;
    if (page == 0x2e)
        fprintf(stderr,
                "Reading TapeAlert consumes the drive's latched flags; retain this output.\n");
    if (!logPage(device, page, data))
        return 1;
    printf("Log page 0x%02x\n", page);
    if (!page) {
        printf("Supported pages:");
        for (size_t i = 4; i < data.size(); ++i)
            printf(" 0x%02x", data[i]);
        puts("");
        return 0;
    }
    std::vector<tape::LogParameter> params;
    if (!tape::decodeLog(data.data(), data.size(), page, params)) {
        fprintf(stderr, "Malformed log parameters\n");
        return 1;
    }
    if (page == 0x2e) {
        unsigned active = 0;
        for (const auto &p : params) {
            if (p.code < 1 || p.code > 64 || p.data.size() != 1 || p.data[0] > 1) {
                fprintf(stderr, "Invalid TapeAlert flag\n");
                return 1;
            }
        }
        if (params.size() != 64) {
            fprintf(stderr, "Incomplete TapeAlert flag set\n");
            return 1;
        }
        for (const auto &p : params)
            if (p.data[0]) {
                printf("  TapeAlert %u: %s\n", p.code, tape::alertName(p.code));
                ++active;
            }
        if (!active)
            puts("  No active TapeAlert flags reported.");
        return active ? 3 : 0;
    }
    const char *errors[] = {
        "corrected without delay", "corrected with retry",  "total errors",
        "corrected errors",        "correction operations", "data sets processed (HP)",
        "uncorrected errors"};
    for (const auto &p : params) {
        uint64_t value = 0;
        printf("  0x%04x", p.code);
        if ((page == 2 || page == 3) && p.code < 7)
            printf(" (%s)", errors[p.code]);
        if (page == 0x0c && p.code < 4) {
            const char *names[] = {"bytes from host", "bytes written to media",
                                   "bytes read from media", "bytes to host"};
            printf(" (%s)", names[p.code]);
        }
        if (page == 0x30 && p.code >= 1 && p.code <= 9) {
            const char *names[] = {"",
                                   "cartridge loads",
                                   "data sets written",
                                   "write retries",
                                   "unrecovered writes",
                                   "suspended writes",
                                   "fatal suspended writes",
                                   "data sets read",
                                   "read retries",
                                   "unrecovered reads"};
            printf(" (%s)", names[p.code]);
        }
        if (page == 0x0c && p.code == 0x100)
            printf(" (cleaning requested; nonzero means yes)");
        if (page == 0x0d && p.code <= 1 && p.data.size() == 2) {
            printf(" (%s temperature): ", p.code ? "reference" : "current");
            if (p.data[1] == 255)
                puts("unavailable");
            else
                printf("%u C\n", p.data[1]);
            continue;
        }
        if (tape::logNumber(p, value))
            printf(": %" PRIu64, value);
        printf(" [hex:");
        for (auto b : p.data)
            printf(" %02x", b);
        puts("]");
    }
    return 0;
}
static int health(Device &device) {
    std::vector<uint8_t> pages;
    if (!logPage(device, 0, pages))
        return 1;
    int result = 0;
    for (uint8_t page : {uint8_t(0x0c), uint8_t(0x0d), uint8_t(2), uint8_t(3), uint8_t(0x30),
                         uint8_t(0x31), uint8_t(0x2e)}) {
        if (std::find(pages.begin() + 4, pages.end(), page) == pages.end()) {
            printf("Log page 0x%02x not supported\n", page);
            continue;
        }
        const int r = showLog(device, page);
        if (r == 1)
            result = 1;
        else if (r == 3 && !result)
            result = 3;
    }
    return result;
}
struct TransferStats {
    bool writing, complete = false;
    uint64_t bytes = 0;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now(), last = start;
    void print(bool final) const {
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        fprintf(stderr, "%s%s %" PRIu64 " bytes in %.2f s (%.2f MiB/s average)%s\n",
                writing ? "Write" : "Read",
                final ? (complete ? " complete" : " stopped") : " progress", bytes, seconds,
                seconds > 0 ? double(bytes) / 1048576.0 / seconds : 0,
                final ? "; tape remains at current position" : "");
    }
    void add(uint64_t n) {
        bytes += n;
        const auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(5)) {
            print(false);
            last = now;
        }
    }
    ~TransferStats() {
        print(true);
    }
};

static int stream(Device &device, bool writing, uint32_t block) {
    if (!ready(device) || !variableMode(device, block))
        return 1;
    std::vector<uint8_t> buffer(block);
    TransferStats stats{writing};
    if (writing) {
        for (;;) {
            size_t used = 0;
            while (used < buffer.size()) {
                ssize_t n = ::read(STDIN_FILENO, buffer.data() + used, buffer.size() - used);
                if (n < 0 && errno == EINTR)
                    continue;
                if (n < 0) {
                    perror("stdin");
                    return 1;
                }
                if (!n)
                    break;
                used += size_t(n);
            }
            if (!used)
                break;
            uint8_t cdb[6];
            tape::recordCDB(cdb, true, uint32_t(used));
            auto r = device.command(cdb, 6, buffer.data(), uint32_t(used), true, 600000);
            if (!r.good() || r.transferred != used) {
                report(r);
                fprintf(
                    stderr,
                    "Write stopped; tape position/data may have advanced. No retry performed.\n");
                return !r.error && r.service == kSCSIServiceResponse_TASK_COMPLETE &&
                               r.status == kSCSITaskStatus_CHECK_CONDITION && r.parsed.valid &&
                               !r.parsed.deferred && r.parsed.eom &&
                               (r.parsed.key == 0 || r.parsed.key == 13)
                           ? 4
                           : 1;
            }
            stats.add(used);
        }
        uint8_t filemark[6] = {0x10, 0, 0, 0, 1, 0};
        auto r = device.command(filemark, 6, nullptr, 0, false, 600000);
        if (!r.good()) {
            report(r);
            fprintf(stderr, "Filemark failed; buffered data and file termination are not "
                            "confirmed. No retry performed.\n");
            return !r.error && r.service == kSCSIServiceResponse_TASK_COMPLETE &&
                           r.status == kSCSITaskStatus_CHECK_CONDITION && r.parsed.valid &&
                           !r.parsed.deferred && r.parsed.eom &&
                           (r.parsed.key == 0 || r.parsed.key == 13)
                       ? 4
                       : 1;
        }
    } else {
        for (;;) {
            uint8_t cdb[6];
            tape::recordCDB(cdb, false, block);
            auto r = device.command(cdb, 6, buffer.data(), block, false, 600000);
            const auto &s = r.parsed;
            const bool check = !r.error && r.service == kSCSIServiceResponse_TASK_COMPLETE &&
                               r.status == kSCSITaskStatus_CHECK_CONDITION && s.valid &&
                               !s.deferred;
            const bool shortRecord = check && s.ili && s.key == 0 && s.informationValid &&
                                     s.information >= 0 && uint64_t(s.information) <= block &&
                                     r.transferred == block - uint64_t(s.information);
            const bool end = check && ((s.filemark && s.key == 0) || s.key == 8);
            if (r.transferred > block || (!r.good() && !shortRecord && !end)) {
                report(r);
                return 1;
            }
            if (r.transferred && !outputAll(buffer.data(), size_t(r.transferred))) {
                perror("stdout");
                return 1;
            }
            stats.add(r.transferred);
            if (end)
                break;
            if (!r.transferred) {
                fprintf(stderr, "Zero-length read without filemark/EOD\n");
                return 1;
            }
        }
    }
    stats.complete = true;
    return 0;
}
int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    uint64_t selected = 0, block = 0, timeout = 0;
    int index = 1;
    while (index < argc && !strncmp(argv[index], "--", 2)) {
        std::string option = argv[index++];
        if (option == "--help") {
            usage();
            return 0;
        }
        if (index == argc) {
            usage();
            return 2;
        }
        if (option == "--device") {
            if (!parseUnsigned(argv[index++], selected)) {
                usage();
                return 2;
            }
        } else if (option == "--block-size") {
            if (!parseUnsigned(argv[index++], block) || !block || block > 8388608) {
                usage();
                return 2;
            }
        } else if (option == "--timeout-ms") {
            if (!parseUnsigned(argv[index++], timeout) || !timeout || timeout > 1800000) {
                usage();
                return 2;
            }
        } else {
            usage();
            return 2;
        }
    }
    if (index == argc) {
        usage();
        return 2;
    }
    const std::string command = argv[index++];
    const bool argument = command == "seek" || command == "log-page";
    const bool counted = command == "filemark" || command == "fsf" || command == "bsf" ||
                         command == "fsr" || command == "bsr";
    uint64_t count = 1;
    if (argument && index == argc) {
        usage();
        return 2;
    }
    if (index < argc &&
        ((!counted && !argument) || !parseUnsigned(argv[index++], count) ||
         (counted && count > 0x7fffff) || (command == "log-page" && count > 0x3f))) {
        usage();
        return 2;
    }
    if (index != argc) {
        usage();
        return 2;
    }
    const std::vector<std::string> valid = {
        "list",     "inquiry", "status", "rewind",   "read",   "write",  "filemark",
        "fsf",      "bsf",     "fsr",    "bsr",      "eod",    "unload", "position",
        "seek",     "health",  "alerts", "log-page", "limits", "flush",  "safe-disconnect",
        "reconnect"};
#if QLE_SHUTDOWN_TESTING
    const bool faultControl = command == "test-shutdown-arm" || command == "test-shutdown-disarm";
#else
    const bool faultControl = false;
#endif
    bool known = faultControl;
    for (const auto &name : valid)
        known |= command == name;
    if (!known) {
        usage();
        return 2;
    }
    auto found = devices();
    io_service_t chosen = 0;
    unsigned matches = 0;
    for (auto service : found) {
        uint64_t id = 0;
        IORegistryEntryGetRegistryEntryID(service, &id);
        if (command == "list") {
            io_name_t name{};
            IORegistryEntryGetName(service, name);
            printf("%" PRIu64 " %s\n", id, name);
        }
        if (!selected || selected == id) {
            chosen = service;
            ++matches;
        }
    }
    if (command == "list") {
        for (auto service : found)
            IOObjectRelease(service);
        return found.empty() ? 2 : 0;
    }
    if (matches != 1) {
        fprintf(stderr, "Expected one QLE2560 SCSI device; found %u. Use list and --device.\n",
                matches);
        for (auto service : found)
            IOObjectRelease(service);
        return 2;
    }
    const bool localControl =
        faultControl || command == "safe-disconnect" || command == "reconnect";
    if (localControl) {
        // Never send adapter-private CDBs through an older driver to the tape.
        int version = 0;
        bool testing = false;
        io_registry_entry_t entry = chosen;
        IOObjectRetain(entry);
        while (entry) {
            auto value =
                IORegistryEntryCreateCFProperty(entry, CFSTR("IOMatchedPersonality"), nullptr, 0);
            if (value && CFGetTypeID(value) == CFDictionaryGetTypeID()) {
                auto dict = (CFDictionaryRef)value;
                auto test = CFDictionaryGetValue(dict, CFSTR("QLE2560ShutdownTesting"));
                auto cls = CFDictionaryGetValue(dict, CFSTR("IOUserClass"));
                if (cls && CFEqual(cls, CFSTR("QLE2560Driver")) && test &&
                    CFEqual(test, CFSTR("1")))
                    testing = true;
                auto cap = CFDictionaryGetValue(dict, CFSTR("QLE2560ControlVersion"));
                if (cls && CFEqual(cls, CFSTR("QLE2560Driver")) && cap &&
                    CFGetTypeID(cap) == CFNumberGetTypeID())
                    CFNumberGetValue((CFNumberRef)cap, kCFNumberIntType, &version);
            }
            if (value)
                CFRelease(value);
            io_registry_entry_t parent = 0;
            if (!version)
                IORegistryEntryGetParentEntry(entry, kIOServicePlane, &parent);
            IOObjectRelease(entry);
            entry = parent;
        }
        if (version != 2 || (faultControl && !testing)) {
            fprintf(stderr, "Attached driver does not support coordinated disconnect. No control "
                            "command sent.\n");
            for (auto service : found)
                IOObjectRelease(service);
            return 1;
        }
        if (timeout && timeout < 90000) {
            fprintf(stderr, "Adapter control requires a timeout of at least 90000 ms.\n");
            for (auto service : found)
                IOObjectRelease(service);
            return 2;
        }
    }
    IdleSleepAssertion power;
    if (!power.acquire()) {
        fprintf(stderr, "Cannot prevent idle system sleep during tape operation\n");
        for (auto service : found)
            IOObjectRelease(service);
        return 1;
    }
    Device device;
    device.timeoutOverride = uint32_t(timeout);
    bool opened = device.open(chosen);
    for (auto service : found)
        IOObjectRelease(service);
    if (!opened)
        return 1;
    if (localControl) {
        uint8_t operation = command == "safe-disconnect" ? 1 : 2;
#if QLE_SHUTDOWN_TESTING
        if (faultControl)
            operation = command == "test-shutdown-arm" ? 3 : 4;
#endif
        uint8_t control[16];
        tape::controlCDB(control, operation);
        auto result = device.command(control, 16, nullptr, 0, false, 90000);
        if (!result.good()) {
            report(result);
            fprintf(stderr, "Adapter operation failed; clean disconnection is NOT confirmed. No "
                            "retry performed.\n");
            return 1;
        }
        if (faultControl)
            puts("Development shutdown deadline injection updated; arming is one-shot. No shutdown "
                 "requested yet.");
        else if (command == "safe-disconnect")
            puts("Safe to disconnect Thunderbolt: commands drained, buffered writes flushed, "
                 "adapter closed. This does not finish an incomplete backup file.");
        else
            puts("Adapter resume requested. Wait for target discovery, then check status before "
                 "tape I/O.");
        return 0;
    }
    uint8_t inquiry[96]{}, inquiryCDB[6] = {0x12, 0, 0, 0, 96, 0};
    auto r = device.command(inquiryCDB, 6, inquiry, 96);
    if (!r.good() || r.transferred < 36 || (inquiry[0] & 0x1f) != 1) {
        report(r);
        fprintf(stderr, "Selected device is not an accessible sequential-access tape drive.\n");
        return 1;
    }
    if (command == "inquiry") {
        printf("%.8s %.16s firmware %.4s\n", inquiry + 8, inquiry + 16, inquiry + 32);
        return 0;
    }
    if (command == "limits") {
        if (!ready(device))
            return 1;
        uint8_t data[6]{}, cdb[6] = {5};
        r = device.command(cdb, 6, data, sizeof(data));
        if (!r.good() || r.transferred < sizeof(data)) {
            report(r);
            return 1;
        }
        printf("minimum_record_bytes=%u maximum_record_bytes=%u granularity_exponent=%u\n",
               unsigned(uint16_t(data[4]) << 8 | data[5]),
               unsigned(uint32_t(data[1]) << 16 | uint32_t(data[2]) << 8 | data[3]),
               unsigned(data[0] & 31));
        return 0;
    }
    if (command == "health")
        return health(device);
    if (command == "alerts" || command == "log-page")
        return showLog(device, command == "alerts" ? 0x2e : uint8_t(count));
    if (command == "read" || command == "write")
        return stream(device, command == "write", uint32_t(block ? block : 8388608));
    uint8_t cdb[6]{};
    if (command == "status") {
        r = device.command(cdb, 6);
        if (r.good())
            puts("Ready");
        else
            report(r);
        return r.good() ? 0 : 1;
    }
    if (!ready(device))
        return 1;
    if (command == "position") {
        tape::Position p;
        return position(device, p) ? 0 : 1;
    }
    if (command == "seek") {
        uint8_t locate[16];
        tape::locateCDB(locate, count);
        r = device.command(locate, 16, nullptr, 0, false, 1800000);
        if (!r.good()) {
            report(r);
            fprintf(stderr, "Seek failed; re-query position before further I/O.\n");
            return 1;
        }
        tape::Position p;
        if (!position(device, p) || !p.objectKnown || p.object != count) {
            fprintf(stderr, "Seek position verification failed\n");
            return 1;
        }
        return 0;
    }
    if (command == "flush")
        cdb[0] = 0x10;
    else if (command == "rewind")
        cdb[0] = 1;
    else if (command == "unload")
        cdb[0] = 0x1b;
    else if (command == "filemark") {
        cdb[0] = 0x10;
        cdb[2] = count >> 16;
        cdb[3] = count >> 8;
        cdb[4] = count;
    } else {
        const bool backward = command == "bsf" || command == "bsr";
        const uint8_t code = command == "eod" ? 3 : (command == "fsf" || command == "bsf") ? 1 : 0;
        tape::spaceCDB(cdb, code,
                       command == "eod" ? 0
                       : backward       ? -int32_t(count)
                                        : int32_t(count));
    }
    r = device.command(cdb, 6, nullptr, 0, false, 1800000);
    if (!r.good())
        report(r);
    return r.good() ? 0 : 1;
}
