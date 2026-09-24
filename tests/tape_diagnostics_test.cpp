// SPDX-License-Identifier: GPL-2.0-only
#include "../protocol/TapeDiagnostics.hpp"
#include <cassert>
int main() {
    uint8_t pos[32]{};
    pos[0] = 0xc0;
    pos[7] = 3;
    pos[8] = 1;
    pos[15] = 9;
    pos[23] = 4;
    tape::Position p;
    assert(!tape::decodePosition(pos, 31, p));
    assert(tape::decodePosition(pos, 32, p));
    assert(p.bop && p.eop && p.objectKnown && p.fileKnown && p.partition == 3 &&
           p.object == 0x0100000000000009ULL && p.file == 4);
    pos[0] = 12;
    assert(tape::decodePosition(pos, 32, p) && !p.objectKnown && !p.fileKnown);
    uint8_t cdb[16];
    tape::locateCDB(cdb, 0xfedcba9876543210ULL);
    assert(cdb[0] == 0x92 && cdb[1] == 0 && cdb[3] == 0 &&
           isp25xx::be64(cdb + 4) == 0xfedcba9876543210ULL && cdb[15] == 0);
    uint8_t log[] = {0x2e, 0, 0, 10, 0, 20, 0x40, 1, 1, 0, 63, 0x40, 1, 0};
    std::vector<tape::LogParameter> params;
    assert(tape::decodeLog(log, sizeof(log), 0x2e, params) && params.size() == 2);
    uint64_t v = 0;
    assert(params[0].code == 20 && tape::logNumber(params[0], v) && v == 1);
    for (size_t n = 0; n < sizeof(log); ++n)
        assert(!tape::decodeLog(log, n, 0x2e, params) && params.empty());
    log[12] = 2;
    assert(!tape::decodeLog(log, sizeof(log), 0x2e, params));
    log[12] = 1;
    log[10] = 20;
    assert(!tape::decodeLog(log, sizeof(log), 0x2e, params));
    log[10] = 63;
    assert(!tape::decodeLog(log, sizeof(log), 3, params));
    tape::LogParameter large{1, 0, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
    assert(tape::logNumber(large, v) && v == UINT64_MAX);
    large.data.push_back(0);
    assert(!tape::logNumber(large, v));
}
