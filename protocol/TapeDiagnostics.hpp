// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "Wire.hpp"
#include <vector>
namespace tape {
inline uint16_t be16(const uint8_t *p) {
    return uint16_t(p[0]) << 8 | p[1];
}
struct Position {
    bool bop = false, eop = false, objectKnown = false, fileKnown = false;
    uint32_t partition = 0;
    uint64_t object = 0, file = 0;
};
inline bool decodePosition(const uint8_t *p, size_t n, Position &out) {
    if (!p || n < 32)
        return false;
    out = {bool(p[0] & 0x80),    bool(p[0] & 0x40),    !(p[0] & 4),          !(p[0] & 8),
           isp25xx::be32(p + 4), isp25xx::be64(p + 8), isp25xx::be64(p + 16)};
    return true;
}
inline void locateCDB(uint8_t (&cdb)[16], uint64_t object) {
    memset(cdb, 0, 16);
    cdb[0] = 0x92; // Current partition, logical objects, synchronous.
    for (unsigned i = 0; i < 8; ++i)
        cdb[4 + i] = uint8_t(object >> (56 - 8 * i));
}
struct LogParameter {
    uint16_t code;
    uint8_t control;
    std::vector<uint8_t> data;
};
inline bool logLength(const uint8_t *p, size_t n, uint8_t page, size_t &end) {
    if (!p || n < 4 || (p[0] & 0x3f) != page || (p[0] & 0x40) || p[1])
        return false;
    end = 4 + be16(p + 2);
    return end <= n;
}
inline bool decodeLog(const uint8_t *p, size_t n, uint8_t page, std::vector<LogParameter> &out) {
    out.clear();
    size_t end = 0;
    if (!logLength(p, n, page, end) || page == 0)
        return false;
    std::vector<LogParameter> parsed;
    for (size_t at = 4; at < end;) {
        if (end - at < 4)
            return false;
        const size_t length = p[at + 3];
        if (length > end - at - 4)
            return false;
        const uint16_t code = be16(p + at);
        if (!parsed.empty() && parsed.back().code >= code)
            return false;
        parsed.push_back({code, p[at + 2], std::vector<uint8_t>(p + at + 4, p + at + 4 + length)});
        at += 4 + length;
    }
    out = std::move(parsed);
    return true;
}
inline bool logNumber(const LogParameter &p, uint64_t &value) {
    if (p.data.empty() || p.data.size() > 8)
        return false;
    value = 0;
    for (auto byte : p.data)
        value = (value << 8) | byte;
    return true;
}
inline const char *alertName(uint16_t code) {
    switch (code) {
    case 1:
        return "Read warning";
    case 2:
        return "Write warning";
    case 3:
        return "Unrecoverable operation";
    case 4:
        return "Media fault";
    case 5:
        return "Read failed";
    case 6:
        return "Write failed";
    case 7:
        return "Media wear";
    case 9:
        return "Write protected";
    case 20:
        return "Cleaning required";
    case 21:
        return "Cleaning recommended";
    case 22:
        return "Cleaning cartridge exhausted";
    case 23:
        return "Wrong cleaning cartridge";
    case 30:
    case 31:
        return "Drive hardware fault";
    case 32:
        return "Interface warning";
    case 33:
        return "Eject cartridge";
    case 36:
        return "Temperature warning";
    case 38:
        return "Predicted drive failure";
    case 39:
        return "Diagnostics requested";
    case 55:
        return "Load/thread failure";
    case 56:
        return "Unload failure";
    default:
        return "See TapeAlert specification for this flag";
    }
}
} // namespace tape
