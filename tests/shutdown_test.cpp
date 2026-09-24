// SPDX-License-Identifier: GPL-2.0-only
#include "../protocol/Shutdown.hpp"
#include <cassert>
#include <cstdio>
int main() {
#if QLE_SHUTDOWN_TESTING
    tape::ShutdownFault fault;
    fault.begin();
    assert(!fault.active);
    fault.armed = true;
    fault.begin();
    assert(fault.active && !fault.armed);
    fault.finish();
    assert(!fault.active);
    fault.begin();
    assert(!fault.active);
    // Withhold a successful internal flush result past the actual budget.
    // A late completion must not clear dirty state or turn failure into success.
    tape::Shutdown held;
    uint8_t selected[8]{};
    held.observeWrite(selected);
    fault.armed = true;
    fault.begin();
    assert(held.begin(500, 10000000));
    assert(held.nextFlush(false) == 0 && fault.active);
    assert(!held.expired(10000499));
    assert(held.expired(10000500));
    fault.finish();
    held.flushed(true);
    assert(held.step == tape::Shutdown::Step::failed && held.dirty[0]);
    fault.begin();
    assert(!fault.active); // Next shutdown is not injected.
#endif
    tape::Shutdown state;
    uint8_t lun0[8]{}, lun7[8] = {0x40, 7};
    state.observeWrite(lun7);
    state.observeWrite(lun0);
    assert(state.begin(100, 1000));
    assert(!state.begin(101, 1000));
    assert(state.nextFlush(true) == -1); // Never flush over an outstanding write.
    assert(state.nextFlush(false) == 0);
    assert(state.nextFlush(false) == -1); // Only one flush in flight.
    state.flushed(true);
    assert(!state.dirty[0] && state.dirty[7]);
    assert(state.nextFlush(false) == 7 && state.luns[7][0] == 0x40);
    state.flushed(false);
    assert(state.step == tape::Shutdown::Step::failed && state.dirty[7]);
    assert(state.nextFlush(false) == -1); // Failure cannot progress to success.
    assert(state.begin(200, 1000));
    assert(state.nextFlush(false) == 7);
    assert(!state.expired(1199));
    assert(state.expired(1200));
    state.flushed(true);
    assert(state.step == tape::Shutdown::Step::failed && state.dirty[7]);
    assert(state.begin(2000, 1000));
    assert(state.nextFlush(false) == 7);
    state.flushed(true);
    assert(state.nextFlush(false) == -1);
    assert(state.step == tape::Shutdown::Step::finished && !state.pending());
    assert(state.begin(4000, 1000));
    assert(state.nextFlush(false) == -1);
    assert(state.step == tape::Shutdown::Step::finished); // No writes: no tape flush.
    uint8_t cdb[16];
    for (uint8_t op = 1; op <= 4; ++op) {
        tape::controlCDB(cdb, op);
        assert(tape::controlOperation(cdb, 16) == op);
        assert(!tape::controlOperation(cdb, 6));
        for (unsigned i = 0; i < 16; ++i) {
            cdb[i] ^= 0x80;
            assert(!tape::controlOperation(cdb, 16));
            cdb[i] ^= 0x80;
        }
    }
    tape::controlCDB(cdb, 5);
    assert(!tape::controlOperation(cdb, 16));
    puts("Shutdown tests passed: drain ordering, multi-LUN flush, timeout/failure, clean "
         "completion, local control validation.");
}
