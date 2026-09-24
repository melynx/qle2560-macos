#include "../protocol/Mailbox.hpp"
#include <cassert>
#include <cstdio>
#include <vector>

struct FakeIO {
    uint64_t time = 0;
    uint32_t initialHost = 0, hccr = 0, completion = 0x40008010;
    bool launched = false;
    std::vector<uint16_t> writes;
    unsigned ackCount = 0, reads = 0;
    uint32_t read32(uint64_t address) {
        if (address == isp25xx::hostStatus)
            return launched ? completion : initialHost;
        return hccr;
    }
    uint16_t read16(uint64_t address) {
        assert(!ackCount); // Firmware results must be captured before acknowledgment.
        ++reads;
        return address == isp25xx::mailboxBase + 2 ? 0xffff : 0xaaaa;
    }
    void write16(uint64_t, uint16_t value) {
        writes.push_back(value);
    }
    void write32(uint64_t address, uint32_t value) {
        assert(address == isp25xx::hostCommand);
        if (value == isp25xx::setHostInterrupt)
            launched = true;
        else {
            assert(value == isp25xx::clearRiscInterrupt);
            ++ackCount;
        }
    }
    uint64_t nowMicros() {
        return time;
    }
    void waitMicros(uint32_t duration) {
        time += duration;
    }
};

struct EventIO : FakeIO {
    unsigned pendingBefore = 1, pendingDuring = 1;
    uint32_t read32(uint64_t address) {
        if (address != isp25xx::hostStatus)
            return hccr;
        if (!launched)
            return pendingBefore ? 0x80108012 : 0;
        return pendingDuring ? 0x00008013 : completion;
    }
    uint16_t read16(uint64_t) {
        ++reads;
        return 0x1234;
    }
    void write32(uint64_t address, uint32_t value) {
        FakeIO::write32(address, value);
        if (value == isp25xx::clearRiscInterrupt) {
            if (!launched && pendingBefore)
                --pendingBefore;
            else if (launched && pendingDuring)
                --pendingDuring;
        }
    }
};

int main() {
    using namespace isp25xx;
    uint16_t input[32] = {6, 0xaaaa, 0x5555}, output[32] = {};
    for (uint32_t kind : {0x01u, 0x10u}) {
        FakeIO io;
        io.completion = 0x40008000 | kind;
        Mailbox<FakeIO> mailbox(io);
        assert(mailbox.execute(input, output, 1000) == MailboxResult::complete);
        assert(output[0] == 0x4000 && output[1] == 0xffff);
        assert(io.writes.size() == 32 && io.writes[2] == 0x5555);
        assert(io.ackCount == 1 && io.reads == 31);
    }
    for (uint32_t status : {0x40058002u, 0x40058011u, 0x40058010u}) {
        FakeIO io;
        io.completion = status;
        Mailbox<FakeIO> mailbox(io);
        assert(mailbox.execute(input, output, 1000) == MailboxResult::firmwareError);
    }
    for (auto result : {MailboxResult::timeout, MailboxResult::disconnected, MailboxResult::halted,
                        MailboxResult::unexpectedEvent}) {
        FakeIO io;
        io.completion = result == MailboxResult::timeout        ? 0
                        : result == MailboxResult::disconnected ? UINT32_MAX
                        : result == MailboxResult::halted       ? riscPaused
                                                                : 0x80108012;
        Mailbox<FakeIO> mailbox(io);
        assert(mailbox.execute(input, output, 1000) == result);
        assert(mailbox.execute(input, output, 1000) == MailboxResult::sessionUnusable);
        assert(io.ackCount == 0 && io.time <= 1000);
    }
    for (bool stale : {true, false}) {
        FakeIO io;
        if (stale)
            io.initialHost = 0x40008010;
        else
            io.hccr = hostInterrupt;
        Mailbox<FakeIO> mailbox(io);
        assert(mailbox.execute(input, output, 1000) == MailboxResult::busy);
        assert(io.writes.empty());
    }
    FakeIO io;
    Mailbox<FakeIO> mailbox(io);
    assert(mailbox.execute(input, output, 0) == MailboxResult::invalidBudget);
    assert(io.writes.empty());
    // Drain an event before launch and a response interrupt while waiting,
    // without confusing either with the mailbox completion.
    EventIO events;
    unsigned callbacks = 0;
    Mailbox<EventIO> operational(events, &callbacks, [](void *context, uint32_t status) {
        ++*static_cast<unsigned *>(context);
        return (status & 0xff) == 0x12 || (status & 0xff) == 0x13;
    });
    assert(operational.execute(input, output, 1000) == MailboxResult::complete);
    assert(callbacks == 2 && events.ackCount == 3 && events.reads == 31);
    assert(output[0] == 0x4000 && output[1] == 0x1234);
    EventIO storm;
    storm.pendingBefore = 33;
    Mailbox<EventIO> bounded(storm, &callbacks, [](void *, uint32_t) { return true; });
    assert(bounded.execute(input, output, 1000) == MailboxResult::busy);
    assert(storm.ackCount == 32 && storm.writes.empty());
    std::puts("Mailbox tests passed: ROM/firmware completion, failure, timeout, disconnect, halt, "
              "async rejection and stale completion.");
}
