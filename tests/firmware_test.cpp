#include "../protocol/Wire.hpp"
#include "../protocol/Reset.hpp"
#include "../generated/QL2500Firmware.hpp"
#include <cassert>
#include <vector>
#include <cstdio>
struct ResetIO {
    uint64_t time = 0;
    uint32_t control = 0;
    bool disconnected = false, stuckDMA = false, stuckReset = false, stuckROM = false;
    unsigned releases = 0;
    uint32_t read32(uint64_t off) {
        if (disconnected)
            return UINT32_MAX;
        if (off == isp25xx::controlStatus)
            return (stuckDMA ? isp25xx::dmaActive : 0) | (stuckReset ? control & 1 : 0);
        return 0;
    }
    uint16_t read16(uint64_t) {
        return stuckROM ? 1 : 0;
    }
    void write32(uint64_t off, uint32_t v) {
        if (off == 8)
            control = v;
        if (off == 0x48)
            ++releases;
    }
    void waitMicros(uint32_t us) {
        time += us;
    }
    uint64_t nowMicros() {
        return time;
    }
};
int main() {
    using namespace isp25xx;
    FirmwareSegment segments[2];
    assert(parseFirmware(firmwareImage, sizeof(firmwareImage), segments));
    assert(segments[0].address == 0x100000 && segments[1].address == 0x116000);
    for (size_t size : {size_t(0), size_t(12), sizeof(firmwareImage) - 4})
        assert(!parseFirmware(firmwareImage, size, segments));
    std::vector<uint8_t> bad(firmwareImage, firmwareImage + sizeof(firmwareImage));
    bad[100] ^= 1;
    assert(!parseFirmware(bad.data(), bad.size(), segments));
    uint8_t nv[512] = {}, icb[128];
    memcpy(nv, "ISP ", 4);
    put16(nv + 4, 1);
    put16(nv + 8, 1);
    nv[20] = 0x21;
    nv[27] = 1;
    nv[28] = 0x20;
    nv[35] = 1;
    uint32_t sum = 0;
    for (unsigned i = 0; i < 508; i += 4)
        sum += le32(nv + i);
    put32(nv + 508, 0 - sum);
    assert(validNvram(nv));
    assert(makeICB(icb, nv, 0x12345678000, 0x23456789000, 256));
    assert(le32(icb + 44) == 0x45678000 && le32(icb + 48) == 0x123);
    assert(le16(icb + 36) == 256 && le32(icb + 96) == 0x1020);
    nv[27] ^= 1;
    assert(!validNvram(nv));
    assert(!makeICB(icb, nv, 4096, 8192, 256));
    ResetIO io;
    assert(resetController(io) == ResetResult::complete);
    assert(io.releases == 3);
    io = {};
    io.stuckDMA = true;
    assert(resetController(io) == ResetResult::dmaTimeout);
    assert(io.releases == 0 && io.time == 300000);
    io = {};
    io.stuckReset = true;
    assert(resetController(io) == ResetResult::resetTimeout);
    io = {};
    io.stuckROM = true;
    assert(resetController(io) == ResetResult::nvramTimeout);
    io = {};
    io.disconnected = true;
    assert(resetController(io) == ResetResult::disconnected);
    assert(io.releases == 0);
    puts("Firmware/ICB/reset tests passed.");
}
