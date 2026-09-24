// SPDX-License-Identifier: GPL-2.0-only
// ISP24xx/25xx mailbox protocol, based on the pinned Linux qla2xxx sources.
#pragma once
#include "ISP25xx.hpp"

namespace isp25xx {
enum class MailboxResult {
    complete,
    firmwareError,
    timeout,
    disconnected,
    halted,
    busy,
    unexpectedEvent,
    invalidBudget,
    sessionUnusable
};

// A synchronous, single-owner transport. IO supplies read32, read16, write32,
// write16, nowMicros and waitMicros. All interrupt consumers and mailbox callers
// must share one serial queue. An event callback captures asynchronous status
// before acknowledgment. DMA ownership belongs to the controller. Ambiguous
// transactions prevent reuse until a verified controller reset.
template <class IO> class Mailbox {
    IO &io;
    bool usable = true;
    void *eventContext = nullptr;
    bool (*eventHandler)(void *, uint32_t) = nullptr;

  public:
    explicit Mailbox(IO &access) : io(access) {}
    Mailbox(IO &access, void *context, bool (*handler)(void *, uint32_t))
        : io(access), eventContext(context), eventHandler(handler) {}
    void resetSession() {
        usable = true;
    } // Only after a verified controller reset.

    MailboxResult execute(const uint16_t (&input)[32], uint16_t (&output)[32],
                          uint32_t timeoutMicros) {
        for (auto &word : output)
            word = 0;
        if (!usable)
            return MailboxResult::sessionUnusable;
        if (!timeoutMicros || timeoutMicros > 30000000)
            return MailboxResult::invalidBudget;
        uint32_t host = io.read32(hostStatus);
        for (unsigned i = 0; eventHandler && i < 32 && host != UINT32_MAX && !(host & riscPaused) &&
                             (host & riscInterrupt);
             ++i) {
            if (!eventHandler(eventContext, host))
                break;
            io.write32(hostCommand, clearRiscInterrupt);
            if (io.read32(hostCommand) == UINT32_MAX) {
                host = UINT32_MAX;
                break;
            }
            host = io.read32(hostStatus);
        }
        const uint32_t command = io.read32(hostCommand);
        if (host == UINT32_MAX || command == UINT32_MAX) {
            usable = false;
            return MailboxResult::disconnected;
        }
        if ((host & riscPaused) || (command & riscReset)) {
            usable = false;
            return MailboxResult::halted;
        }
        // Never mistake a completion from an earlier owner for our command.
        if ((host & riscInterrupt) || (command & hostInterrupt)) {
            usable = false;
            return MailboxResult::busy;
        }
        usable = false;
        const uint64_t start = io.nowMicros();
        for (unsigned i = 0; i < 32; ++i)
            io.write16(mailboxBase + i * 2, input[i]);
        io.write32(hostCommand, setHostInterrupt);
        if (io.read32(hostCommand) == UINT32_MAX)
            return MailboxResult::disconnected;
        for (;;) {
            if (io.nowMicros() - start >= timeoutMicros)
                return MailboxResult::timeout;
            const uint32_t status = io.read32(hostStatus);
            if (status == UINT32_MAX)
                return MailboxResult::disconnected;
            if (status & riscPaused)
                return MailboxResult::halted;
            if (status & riscInterrupt) {
                const uint8_t kind = status & 0xff;
                if (kind != 0x01 && kind != 0x02 && kind != 0x10 && kind != 0x11) {
                    if (!eventHandler || !eventHandler(eventContext, status))
                        return MailboxResult::unexpectedEvent;
                    io.write32(hostCommand, clearRiscInterrupt);
                    if (io.read32(hostCommand) == UINT32_MAX)
                        return MailboxResult::disconnected;
                    io.waitMicros(100);
                    continue;
                }
                // Completion mailbox zero comes from the interrupt status.
                output[0] = status >> 16;
                for (unsigned i = 1; i < 32; ++i)
                    output[i] = io.read16(mailboxBase + i * 2);
                // All ones is valid mailbox data; check the device separately.
                if (io.read32(hostCommand) == UINT32_MAX)
                    return MailboxResult::disconnected;
                io.write32(hostCommand, clearRiscInterrupt);
                if (io.read32(hostCommand) == UINT32_MAX)
                    return MailboxResult::disconnected;
                usable = true;
                return output[0] == 0x4000 && (kind == 0x01 || kind == 0x10)
                           ? MailboxResult::complete
                           : MailboxResult::firmwareError;
            }
            io.waitMicros(100);
        }
    }
};
} // namespace isp25xx
