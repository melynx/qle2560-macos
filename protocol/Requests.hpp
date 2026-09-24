// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "FCP.hpp"
namespace isp25xx {
struct Requests {
    static constexpr unsigned taskCount = 32;
    struct Task {
        bool active = false, done = false, timeoutAbort = false;
        uint32_t generation = 0, requested = 0;
        uint64_t deadline = 0;
        Completion completion{};
    } tasks[taskCount];
    int senseTask = -1;
    uint32_t begin(unsigned slot, uint32_t bytes, uint64_t deadline) {
        if (slot >= taskCount || tasks[slot].active)
            return 0;
        auto &task = tasks[slot];
        if (++task.generation >= 0x1000000)
            task.generation = 1;
        task.completion = {};
        task.completion.handle = (task.generation << 8) | (slot + 1);
        task.timeoutAbort = false;
        task.requested = bytes;
        task.deadline = deadline;
        task.active = true;
        task.done = false;
        return task.completion.handle;
    }
    bool consume(const uint8_t (&entry)[64]) {
        if (senseTask >= 0) {
            auto &task = tasks[senseTask];
            if (!appendSense(entry, task.completion))
                return false;
            if (task.completion.senseCopied == task.completion.senseLength) {
                task.done = true;
                senseTask = -1;
            }
            return true;
        }
        const uint32_t handle = le32(entry + 4);
        const unsigned slot = (handle & 0xff) - 1;
        if (slot >= taskCount || !tasks[slot].active || tasks[slot].done ||
            tasks[slot].completion.handle != handle)
            return false;
        auto &task = tasks[slot];
        if (!decodeStatus(entry, task.requested, task.completion))
            return false;
        if (task.completion.senseCopied < task.completion.senseLength)
            senseTask = int(slot);
        else
            task.done = true;
        return true;
    }
    bool takeCompletion(unsigned slot, Completion &completion) {
        if (slot >= taskCount || !tasks[slot].active || !tasks[slot].done)
            return false;
        completion = tasks[slot].completion;
        if (tasks[slot].timeoutAbort)
            completion.result = IOResult::timeout;
        tasks[slot].active = tasks[slot].done = false;
        return true;
    }
    bool startTimeoutAbort(unsigned slot, uint64_t now, uint64_t grace) {
        if (slot >= taskCount)
            return false;
        auto &task = tasks[slot];
        if (!task.active || task.done || task.timeoutAbort || now < task.deadline)
            return false;
        task.timeoutAbort = true;
        task.deadline = now + grace;
        return true;
    }
    bool expired(uint64_t now) const {
        for (const auto &task : tasks)
            if (task.active && !task.done && now >= task.deadline)
                return true;
        return false;
    }
    // Only the hardware owner can establish quiescence before discarding tasks.
    void clearAfterQuiesce() {
        for (auto &task : tasks)
            task.active = task.done = false;
        senseTask = -1; // Keep generation counters across reset.
    }
};
} // namespace isp25xx
