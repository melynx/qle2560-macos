#include "../protocol/FCP.hpp"
#include "../protocol/Tape.hpp"
#include <cassert>
#include <cstdio>
#include <climits>
int main() {
    uint8_t report[64]{};
    report[0] = 0x32;
    report[1] = 1;
    assert(isp25xx::isPortIDReport(report));
    report[15] = 2;
    assert(isp25xx::isPortIDReport(report));
    report[10] = 1;
    assert(!isp25xx::isPortIDReport(report));
    report[10] = 0;
    report[3] = 1;
    assert(!isp25xx::isPortIDReport(report));
    report[3] = 0;
    report[0] = 3;
    assert(!isp25xx::isPortIDReport(report));

    using namespace isp25xx;
    uint8_t entry[64]{}, lun[8] = {0, 7}, cdb[16] = {0x12, 0, 0, 0, 96};
    assert(
        makeCommand(entry, 0x123401, 5, 0x123456, lun, cdb, 6, 0x123456789abcULL, 96, 1, 0, 60000));
    assert(entry[0] == 0x18 && entry[1] == 1 && le32(entry + 4) == 0x123401);
    assert(entry[18] == 7 && entry[31] == 0x12 && entry[35] == 96);
    assert(le16(entry + 24) == 2 && le32(entry + 52) == 0x56789abc && le32(entry + 56) == 0x1234);
    assert(entry[48] == 0x56 && entry[49] == 0x34 && entry[50] == 0x12);
    assert(!makeCommand(entry, 1, 5, 0, lun, cdb, 17, 4096, 96, 1, 0, 60000));
    assert(!makeCommand(entry, 1, 5, 0, lun, cdb, 6, UINT64_MAX, 96, 1, 0, 60000));
    memset(entry, 0, 64);
    entry[0] = 3;
    entry[1] = 1;
    put32(entry + 4, 1);
    Completion done;
    assert(decodeStatus(entry, 1000, done) && done.transferred == 1000 &&
           done.result == IOResult::complete);
    put16(entry + 8, 0x15);
    put16(entry + 22, 0x800);
    put32(entry + 12, 64);
    put32(entry + 24, 64);
    assert(decodeStatus(entry, 1000, done) && done.transferred == 936 &&
           done.result == IOResult::complete);
    put32(entry + 12, 65);
    assert(decodeStatus(entry, 1000, done) && done.result == IOResult::transportError);
    put32(entry + 24, 1001);
    assert(!decodeStatus(entry, 1000, done));
    memset(entry, 0, 64);
    entry[0] = 3;
    entry[1] = 1;
    put32(entry + 4, 1);
    put16(entry + 22, 0x202);
    put32(entry + 28, 90);
    for (unsigned i = 0; i < 28; ++i)
        entry[36 + i] = i;
    swapWords(entry + 36, 28);
    assert(decodeStatus(entry, 1000, done) && done.senseLength == 90 && done.senseCopied == 28);
    uint8_t continuation[64]{};
    continuation[0] = 0x10;
    continuation[1] = 1;
    for (unsigned i = 0; i < 60; ++i)
        continuation[4 + i] = i + 28;
    swapWords(continuation + 4, 60);
    assert(appendSense(continuation, done) && done.senseCopied == 88);
    continuation[7] = 88;
    continuation[6] = 89;
    assert(appendSense(continuation, done) && done.senseCopied == 90);
    for (unsigned i = 0; i < 90; ++i)
        assert(done.sense[i] == i);
    assert(!appendSense(continuation, done));
    put32(entry + 28, UINT32_MAX);
    assert(!decodeStatus(entry, 1000, done));
    put32(entry + 28, 0);
    put16(entry + 22, 0x100);
    put32(entry + 32, UINT32_MAX);
    assert(!decodeStatus(entry, 1000, done));
    uint8_t sense[32]{};
    sense[0] = 0xf0;
    sense[2] = 0xa0;
    sense[6] = 20;
    sense[7] = 10;
    auto s = tape::decodeSense(sense, 18);
    assert(s.valid && s.filemark && s.ili && s.informationValid && s.information == 20);
    sense[0] = 0x72;
    sense[1] = 0;
    sense[7] = 4;
    sense[8] = 4;
    sense[9] = 2;
    sense[11] = 0xc0;
    s = tape::decodeSense(sense, 12);
    assert(s.valid && s.filemark && s.eom);
    sense[9] = 40;
    assert(!tape::decodeSense(sense, 12).valid);
    uint8_t record[6];
    assert(tape::recordCDB(record, true, 262144));
    assert(record[0] == 10 && record[2] == 4);
    assert(!tape::recordCDB(record, true, 0));
    assert(!tape::recordCDB(record, true, 0x1000000));
    assert(tape::spaceCDB(record, 1, -1));
    assert(record[2] == 255 && record[3] == 255 && record[4] == 255);
    assert(!tape::spaceCDB(record, 1, INT_MAX));
    puts("FCP/tape tests passed: IOCB byte order, residuals, sense continuation, malformed input, "
         "record and spacing CDBs.");
}
