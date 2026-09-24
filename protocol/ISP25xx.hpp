// SPDX-License-Identifier: GPL-2.0-only
// Register layout derived from Linux qla2xxx/qla_fw.h (device_reg_24xx).
#pragma once
#include <stddef.h>
#include <stdint.h>

namespace isp25xx {
struct RegisterLayout {
    uint32_t flashAddress, flashData, controlStatus, interruptControl, interruptStatus;
    uint32_t reserved1[2];
    uint32_t requestIn, requestOut, responseIn, responseOut, priorityIn, priorityOut;
    uint32_t reserved2[2];
    uint32_t atioIn, atioOut, hostStatus, hostCommand, gpioData, gpioEnable, ioBase;
    uint32_t reserved3[10];
    uint16_t mailbox[32];
};
static_assert(offsetof(RegisterLayout, controlStatus) == 0x08);
static_assert(offsetof(RegisterLayout, hostStatus) == 0x44);
static_assert(offsetof(RegisterLayout, hostCommand) == 0x48);
static_assert(offsetof(RegisterLayout, mailbox) == 0x80);
static_assert(sizeof(RegisterLayout) == 0xc0);
constexpr uint64_t controlStatus = 0x08, interruptControl = 0x0c;
constexpr uint64_t hostStatus = 0x44, hostCommand = 0x48, mailboxBase = 0x80;
constexpr uint32_t riscInterrupt = 1u << 15, riscPaused = 1u << 8;
constexpr uint32_t dmaActive = 1u << 17, softReset = 1u;
constexpr uint32_t riscReset = 1u << 5, hostInterrupt = 1u << 6;
constexpr uint32_t setHostInterrupt = 0x50000000, clearRiscInterrupt = 0xa0000000;

// Subtraction avoids an offset+width overflow. The driver uses this
// check for the mailbox range before using the fixed register addresses.
constexpr bool validAccess(uint64_t size, uint64_t offset, uint64_t width) {
    return (width == 2 || width == 4) && (offset % width == 0) && offset <= size &&
           width <= size - offset;
}
} // namespace isp25xx
