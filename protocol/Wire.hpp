// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
namespace isp25xx {
inline uint16_t le16(const uint8_t *p) {
    return uint16_t(p[0]) | uint16_t(p[1]) << 8;
}
inline uint32_t le32(const uint8_t *p) {
    return uint32_t(le16(p)) | uint32_t(le16(p + 2)) << 16;
}
inline uint32_t be32(const uint8_t *p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline uint64_t be64(const uint8_t *p) {
    return uint64_t(be32(p)) << 32 | be32(p + 4);
}
inline void put16(uint8_t *p, uint16_t v) {
    p[0] = v;
    p[1] = v >> 8;
}
inline void put32(uint8_t *p, uint32_t v) {
    put16(p, v);
    put16(p + 2, v >> 16);
}
inline void put64(uint8_t *p, uint64_t v) {
    put32(p, uint32_t(v));
    put32(p + 4, uint32_t(v >> 32));
}
inline void swapWords(uint8_t *p, size_t length) {
    for (size_t i = 0; i + 4 <= length; i += 4) {
        uint8_t a = p[i], b = p[i + 1];
        p[i] = p[i + 3];
        p[i + 1] = p[i + 2];
        p[i + 2] = b;
        p[i + 3] = a;
    }
}
inline void mailboxDMA(uint16_t (&m)[32], uint64_t address) {
    m[2] = uint16_t(address >> 16);
    m[3] = uint16_t(address);
    m[6] = uint16_t(address >> 48);
    m[7] = uint16_t(address >> 32);
}
struct FirmwareSegment {
    uint32_t address = 0, words = 0;
    size_t offset = 0;
};
inline bool parseFirmware(const uint8_t *data, size_t size, FirmwareSegment (&segments)[2]) {
    if (!data || size < 80 || size % 4)
        return false;
    size_t cursor = 0;
    for (auto &segment : segments) {
        if (size - cursor < 40)
            return false;
        segment = {be32(data + cursor + 8), be32(data + cursor + 12), cursor};
        if (segment.address < 0x100000 || segment.address >= 0x200000 || segment.words < 10 ||
            segment.words > (size - cursor) / 4 || segment.words > 0x200000 - segment.address)
            return false;
        uint32_t sum = 0;
        for (size_t i = 0; i < segment.words; ++i)
            sum += be32(data + cursor + i * 4);
        if (sum)
            return false;
        cursor += size_t(segment.words) * 4;
    }
    if (cursor != size)
        return false;
    return segments[0].address + segments[0].words <= segments[1].address ||
           segments[1].address + segments[1].words <= segments[0].address;
}
inline bool validNvram(const uint8_t (&nv)[512]) {
    if (memcmp(nv, "ISP ", 4) || le16(nv + 4) < 1 || le16(nv + 8) < 1)
        return false;
    uint32_t sum = 0;
    for (unsigned i = 0; i < 512; i += 4)
        sum += le32(nv + i);
    if (sum)
        return false;
    const uint8_t *port = nv + 20, *node = nv + 28;
    if (le32(nv + 256) & 0x8000) {
        port = nv + 260;
        node = nv + 268;
    }
    return be64(port) != 0 && be64(port) != UINT64_MAX &&
           (!(le32(nv + 44) & 0x4000) || (be64(node) != 0 && be64(node) != UINT64_MAX));
}
// Exact 128-byte ISP24xx initialization control block. Queue addresses are IOVAs.
inline bool makeICB(uint8_t (&icb)[128], const uint8_t (&nv)[512], uint64_t request,
                    uint64_t response, uint16_t depth) {
    if (!validNvram(nv) || depth < 16 || depth > 1024 || !request || !response || request % 64 ||
        response % 64)
        return false;
    memset(icb, 0, sizeof(icb));
    put16(icb, 1);
    put16(icb + 4, 2048);
    put16(icb + 6, 0xffff);
    put16(icb + 10, 124);
    const bool alternate = (le32(nv + 256) & 0x8000) != 0;
    memcpy(icb + 12, nv + (alternate ? 260 : 20), 8);
    memcpy(icb + 20, nv + (alternate ? 268 : 28), 8);
    if (!(le32(nv + 44) & 0x4000)) {
        memcpy(icb + 20, icb + 12, 8);
        icb[20] &= 0xf0;
    }
    put16(icb + 32, 8);
    put16(icb + 36, depth);
    put16(icb + 38, depth);
    put16(icb + 40, le16(nv + 38));
    put64(icb + 44, request);
    put64(icb + 52, response);
    put16(icb + 90, 4);
    put32(icb + 92, (1u << 14) | (1u << 13) | (1u << 2) | (1u << 1));
    // Loop then point-to-point, FC tape enabled; automatic PLOGI permitted.
    put32(icb + 96, (2u << 4) | (1u << 12));
    put32(icb + 100, 2u << 13); // Automatic link speed; no index shadowing.
    return true;
}
} // namespace isp25xx
