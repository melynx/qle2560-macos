// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "Wire.hpp"
namespace isp25xx {
// Firmware posts ID acquisition independently of host SCSI requests.
inline bool isPortIDReport(const uint8_t (&entry)[64]) {
    return entry[0] == 0x32 && entry[1] == 1 && entry[3] == 0 && entry[15] <= 2 &&
           (entry[15] == 0 || entry[10] == 0);
}

enum class IOResult { complete, transportError, timeout, aborted, reset, noDevice, overrun };
struct Completion {
    uint32_t handle = 0;
    IOResult result = IOResult::transportError;
    uint8_t status = 0;
    uint32_t transferred = 0;
    uint8_t sense[252]{};
    uint32_t senseLength = 0, senseCopied = 0;
};
inline bool makeCommand(uint8_t (&entry)[64], uint32_t handle, uint16_t nport, uint32_t port,
                        const uint8_t *lun, const uint8_t *cdb, uint8_t cdbLength, uint64_t dma,
                        uint32_t bytes, uint8_t direction, uint8_t attribute, uint32_t timeoutMs,
                        uint8_t crn = 0) {
    if (!handle || nport >= 0x7f0 || port > 0xffffff || !lun || !cdb || cdbLength == 0 ||
        cdbLength > 16 || direction > 2 || attribute > 5 || attribute == 3 ||
        (bytes && (!dma || !direction || dma > UINT64_MAX - bytes)))
        return false;
    memset(entry, 0, 64);
    entry[0] = 0x18;
    entry[1] = 1;
    put32(entry + 4, handle);
    put16(entry + 8, nport);
    uint64_t seconds = (uint64_t(timeoutMs) + 999) / 1000;
    put16(entry + 10, seconds <= 0x1999 ? uint16_t(seconds) : 0);
    put16(entry + 12, bytes ? 1 : 0);
    memcpy(entry + 16, lun, 8);
    swapWords(entry + 16, 8);
    put16(entry + 24, bytes ? (direction == 1 ? 2 : 1) : 0);
    entry[26] = attribute;
    entry[27] = crn;
    memcpy(entry + 28, cdb, cdbLength);
    swapWords(entry + 28, 16);
    put32(entry + 44, bytes);
    entry[48] = port;
    entry[49] = port >> 8;
    entry[50] = port >> 16;
    if (bytes) {
        put64(entry + 52, dma);
        put32(entry + 60, bytes);
    }
    return true;
}
// The caller checks generation-tagged handle ownership before applying output.
inline bool decodeStatus(const uint8_t (&entry)[64], uint32_t requested, Completion &out) {
    if (entry[0] != 3 || !entry[1] || entry[3])
        return false;
    out = {};
    out.handle = le32(entry + 4);
    out.status = entry[22];
    const uint16_t completion = le16(entry + 8), status = le16(entry + 22);
    uint32_t residual = 0;
    out.result = IOResult::complete;
    switch (completion) {
    case 0:
        break;
    case 0x15:
        residual = le32(entry + 12);
        if (!(status & 0x800) || residual != le32(entry + 24))
            out.result = IOResult::transportError;
        break;
    case 0x1c:
        out.status = 0x28;
        break;
    case 4:
        out.result = IOResult::reset;
        break;
    case 5:
    case 0x13:
    case 0x47:
        out.result = IOResult::aborted;
        break;
    case 6:
        out.result = IOResult::timeout;
        break;
    case 7:
        out.result = IOResult::overrun;
        break;
    case 0x28:
    case 0x29:
    case 0x2a:
        out.result = IOResult::noDevice;
        break;
    default:
        out.result = IOResult::transportError;
        break;
    }
    if (status & 0x800)
        residual = le32(entry + 24);
    if (status & 0x400)
        out.result = IOResult::overrun;
    if (residual > requested)
        return false;
    out.transferred = out.result == IOResult::complete ? requested - residual : 0;
    uint8_t data[28];
    memcpy(data, entry + 36, 28);
    swapWords(data, 28);
    const uint32_t responseLength = (status & 0x100) ? le32(entry + 32) : 0;
    if (responseLength > 28 || (responseLength && responseLength < 4))
        return false;
    if (responseLength && data[3])
        out.result = IOResult::transportError;
    if (status & 0x200) {
        const uint32_t reported = le32(entry + 28);
        // Bound the full continuation chain as well as the caller's sense copy.
        if (reported > 252)
            return false;
        out.senseLength = reported;
        out.senseCopied = reported < 28 - responseLength ? reported : 28 - responseLength;
        memcpy(out.sense, data + responseLength, out.senseCopied);
    }
    return true;
}
inline bool appendSense(const uint8_t (&entry)[64], Completion &out) {
    if (entry[0] != 0x10 || entry[3] || out.senseCopied >= out.senseLength)
        return false;
    uint8_t data[60];
    memcpy(data, entry + 4, 60);
    swapWords(data, 60);
    uint32_t count = out.senseLength - out.senseCopied;
    if (count > 60)
        count = 60;
    memcpy(out.sense + out.senseCopied, data, count);
    out.senseCopied += count;
    return true;
}
} // namespace isp25xx
