// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "Wire.hpp"
namespace tape {
struct Sense {
    bool valid = false, filemark = false, eom = false, ili = false, informationValid = false,
         deferred = false;
    uint8_t key = 0, asc = 0, ascq = 0;
    int64_t information = 0;
};
inline Sense decodeSense(const uint8_t *p, size_t size) {
    Sense s;
    if (!p || size < 8)
        return s;
    const uint8_t format = p[0] & 0x7f;
    if (format == 0x70 || format == 0x71) {
        s.valid = true;
        s.deferred = format == 0x71;
        s.key = p[2] & 15;
        s.filemark = p[2] & 0x80;
        s.eom = p[2] & 0x40;
        s.ili = p[2] & 0x20;
        s.informationValid = p[0] & 0x80;
        s.information = int32_t(isp25xx::be32(p + 3));
        if (size >= 14 && p[7] >= 6) {
            s.asc = p[12];
            s.ascq = p[13];
        }
    } else if (format == 0x72 || format == 0x73) {
        s.valid = true;
        s.deferred = format == 0x73;
        s.key = p[1] & 15;
        s.asc = p[2];
        s.ascq = p[3];
        size_t end = 8 + p[7];
        if (end > size)
            end = size;
        for (size_t cursor = 8; cursor + 2 <= end;) {
            const size_t length = p[cursor + 1] + 2;
            if (length > end - cursor) {
                s.valid = false;
                break;
            }
            if (p[cursor] == 4 && length >= 4) {
                s.filemark = p[cursor + 3] & 0x80;
                s.eom = p[cursor + 3] & 0x40;
                s.ili = p[cursor + 3] & 0x20;
            }
            if (p[cursor] == 0 && length >= 12) {
                s.informationValid = p[cursor + 2] & 0x80;
                s.information = int64_t(isp25xx::be64(p + cursor + 4));
            }
            cursor += length;
        }
    }
    return s;
}
inline bool recordCDB(uint8_t (&cdb)[6], bool write, uint32_t bytes) {
    if (!bytes || bytes > 0xffffff)
        return false;
    memset(cdb, 0, 6);
    cdb[0] = write ? 0x0a : 0x08;
    cdb[2] = bytes >> 16;
    cdb[3] = bytes >> 8;
    cdb[4] = bytes;
    return true;
}
inline bool spaceCDB(uint8_t (&cdb)[6], uint8_t code, int32_t count) {
    if (code > 3 || count < -0x800000 || count > 0x7fffff)
        return false;
    memset(cdb, 0, 6);
    cdb[0] = 0x11;
    cdb[1] = code;
    const uint32_t value = uint32_t(count);
    cdb[2] = value >> 16;
    cdb[3] = value >> 8;
    cdb[4] = value;
    return true;
}
} // namespace tape
