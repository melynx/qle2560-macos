// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "Wire.hpp"
namespace tape {
// Local adapter control only. Never forward these commands to a tape drive.
inline void controlCDB(uint8_t (&cdb)[16], uint8_t operation) {
    const uint8_t signature[16] = {0xc0, 'Q', 'L', 'E', '2', '5', '6', '0', 1, 0, 0, 0, 0, 0, 0, 0};
    memcpy(cdb, signature, 16);
    cdb[9] = operation;
}
inline uint8_t controlOperation(const uint8_t *cdb, unsigned length) {
    if (length != 16 || (cdb[9] < 1 || cdb[9] > 4))
        return 0;
    uint8_t expected[16];
    controlCDB(expected, cdb[9]);
    return memcmp(cdb, expected, 16) ? 0 : cdb[9];
}
// Operations 3/4 are reserved test controls. Release drivers reject them locally.
#if QLE_SHUTDOWN_TESTING
struct ShutdownFault {
    bool armed = false, active = false;
    void begin() {
        active = armed;
        armed = false;
    }
    void finish() {
        active = false;
    }
};
#endif
struct Shutdown {
    enum class Step { idle, draining, flushing, finished, failed };
    Step step = Step::idle;
    uint64_t deadline = 0;
    bool dirty[256]{};
    uint8_t luns[256][8]{};
    unsigned cursor = 0;
    bool pending() const {
        return step == Step::draining || step == Step::flushing;
    }
    bool begin(uint64_t now, uint64_t budget) {
        if (pending())
            return false;
        step = Step::draining;
        deadline = now + budget;
        cursor = 0;
        return true;
    }
    void observeWrite(const uint8_t *lun) {
        // Caller validates peripheral/flat LUN addressing, 0..255.
        dirty[lun[1]] = true;
        memcpy(luns[lun[1]], lun, 8);
    }
    bool expired(uint64_t now) {
        if (pending() && now >= deadline)
            step = Step::failed;
        return step == Step::failed;
    }
    int nextFlush(bool busy) {
        if (step != Step::draining || busy)
            return -1;
        while (cursor < 256 && !dirty[cursor])
            ++cursor;
        if (cursor == 256) {
            step = Step::finished;
            return -1;
        }
        step = Step::flushing;
        return int(cursor);
    }
    void flushed(bool good) {
        if (step != Step::flushing)
            return;
        if (!good) {
            step = Step::failed;
            return;
        }
        dirty[cursor] = false;
        ++cursor;
        step = Step::draining;
    }
};
} // namespace tape
