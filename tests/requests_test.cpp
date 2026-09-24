#include "../protocol/Requests.hpp"
#include <cassert>
#include <cstdio>
int main() {
    isp25xx::Requests requests;
    const uint32_t original = requests.begin(0, 512, 1000);
    assert(original && !requests.begin(0, 512, 1000));
    for (unsigned i = 1; i < 32; ++i)
        assert(requests.begin(i, 512, 1000));
    assert(!requests.begin(32, 512, 1000));
    assert(!requests.expired(999) && requests.expired(1000));
    uint8_t entry[64]{};
    entry[0] = 3;
    entry[1] = 1;
    isp25xx::put32(entry + 4, original);
    assert(requests.consume(entry));
    assert(!requests.consume(entry));
    isp25xx::Completion result;
    assert(requests.takeCompletion(0, result) && result.transferred == 512);
    assert(!requests.takeCompletion(0, result));
    assert(requests.begin(0, 512, 2000) != original);
    assert(!requests.consume(entry)); // Late completion must not complete a reused slot.
    requests.clearAfterQuiesce();
    assert(!requests.expired(UINT64_MAX));
    assert(!requests.consume(entry));
    auto next = requests.begin(0, 512, 3000);
    assert(next != original);
    isp25xx::put32(entry + 4, next);
    isp25xx::put16(entry + 22, 0x202);
    isp25xx::put32(entry + 28, 29);
    assert(requests.consume(entry));
    assert(!requests.takeCompletion(0, result));
    assert(!requests.consume(entry)); // Continuation cannot be replaced by another status.
    uint8_t continuation[64]{};
    continuation[0] = 0x10;
    continuation[1] = 1;
    continuation[7] = 0x70;
    assert(requests.consume(continuation));
    assert(requests.takeCompletion(0, result));
    assert(result.senseLength == 29 && result.sense[28] == 0x70);
    requests.clearAfterQuiesce();
    next = requests.begin(0, 512, 100);
    assert(!requests.startTimeoutAbort(0, 99, 1000));
    assert(requests.startTimeoutAbort(0, 100, 1000));
    assert(requests.tasks[0].active && !requests.tasks[0].done);
    assert(!requests.takeCompletion(0, result)); // Accepted abort alone cannot release DMA.
    assert(!requests.startTimeoutAbort(0, 101, 1000));
    assert(!requests.expired(1099) && requests.expired(1100));
    memset(entry, 0, 64);
    entry[0] = 3;
    entry[1] = 1;
    isp25xx::put32(entry + 4, next);
    assert(requests.consume(entry)); // Even GOOD racing with abort remains a timeout to the caller.
    assert(requests.takeCompletion(0, result) && result.result == isp25xx::IOResult::timeout);
    next = requests.begin(0, 512, 2000);
    assert(!requests.tasks[0].timeoutAbort);
    assert(requests.startTimeoutAbort(0, 2000, 1000));
    requests.clearAfterQuiesce();
    assert(!requests.takeCompletion(0, result) && !requests.expired(UINT64_MAX));
    puts("Request ownership tests passed: saturation, duplicate/stale completion, reset "
         "generations, deadlines and sense lifetime.");
}
