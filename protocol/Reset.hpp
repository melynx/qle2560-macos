// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "ISP25xx.hpp"
namespace isp25xx {
enum class ResetResult {
    complete,
    disconnected,
    dmaTimeout,
    nvramTimeout,
    resetTimeout,
    riscTimeout
};
template <class IO> ResetResult resetController(IO &io) {
    auto wait32 = [&](uint64_t offset, uint32_t mask, uint32_t target, uint64_t limit) {
        const auto start = io.nowMicros();
        do {
            const auto value = io.read32(offset);
            if (value == UINT32_MAX)
                return -1;
            if ((value & mask) == target)
                return 1;
            io.waitMicros(10);
        } while (io.nowMicros() - start < limit);
        return 0;
    };
    auto waitMailbox = [&](uint64_t limit) {
        const auto start = io.nowMicros();
        do {
            if (io.read32(controlStatus) == UINT32_MAX)
                return -1;
            if (io.read16(mailboxBase) == 0)
                return 1;
            io.waitMicros(100);
        } while (io.nowMicros() - start < limit);
        return 0;
    };
    if (io.read32(controlStatus) == UINT32_MAX)
        return ResetResult::disconnected;
    io.write32(interruptControl, 0);
    io.write32(controlStatus, 0x10030);
    int ready = wait32(controlStatus, dmaActive, 0, 300000);
    if (ready != 1)
        return ready < 0 ? ResetResult::disconnected : ResetResult::dmaTimeout;
    io.write32(controlStatus, 0x10031);
    io.read32(controlStatus); // Flush posted write before delay.
    io.waitMicros(100);
    ready = waitMailbox(50000);
    if (ready != 1)
        return ready < 0 ? ResetResult::disconnected : ResetResult::nvramTimeout;
    ready = wait32(controlStatus, softReset, 0, 10000);
    if (ready != 1)
        return ready < 0 ? ResetResult::disconnected : ResetResult::resetTimeout;
    const uint32_t commands[] = {0x10000000u, 0x40000000u, 0x20000000u};
    for (uint32_t command : commands) {
        io.write32(hostCommand, command);
        if (io.read32(hostCommand) == UINT32_MAX)
            return ResetResult::disconnected;
    }
    io.waitMicros(10000);
    ready = waitMailbox(300000);
    return ready == 1  ? ResetResult::complete
           : ready < 0 ? ResetResult::disconnected
                       : ResetResult::riscTimeout;
}
} // namespace isp25xx
