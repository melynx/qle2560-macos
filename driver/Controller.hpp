// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include "Hardware.hpp"
#include "../protocol/Wire.hpp"
#include "../protocol/Reset.hpp"
#include "../protocol/Mailbox.hpp"
#include "../protocol/FCP.hpp"
#include "../protocol/Requests.hpp"
#include "../generated/QL2500Firmware.hpp"

class Controller : public isp25xx::Requests {
  public:
    PCIRegisters io;
    isp25xx::Mailbox<PCIRegisters> mailbox;
    DMABuffer scratch, requestRing, responseRing;
    uint8_t nvram[512]{};
    uint16_t lastMailbox[32]{};
    uint16_t topology = 0xffff, ownHandle = 0xffff;
    uint32_t portID = 0;
    uint16_t firmwareState = 0xffff;
    uint16_t lastEvent = 0;
    bool initialized = false, fatal = false, linkChanged = false;
    static constexpr uint16_t queueDepth = 256;
    uint16_t requestIn = 0, responseOut = 0;
    uint16_t targetHandle = 0xffff;
    uint32_t targetPort = 0;
    uint64_t targetWWPN = 0, targetWWNN = 0;
    bool targetReady = false;
    Controller(IOPCIDevice *pci, uint8_t index) : io{pci, index}, mailbox(io, this, handleEvent) {}

    static bool handleEvent(void *context, uint32_t status) {
        auto *self = static_cast<Controller *>(context);
        if ((status & 0xff) == 0x13)
            return true; // Response ring drained by poll().
        if ((status & 0xff) != 0x12)
            return false;
        const uint16_t code = status >> 16;
        self->lastEvent = code;
        uint16_t m1 = self->io.read16(0x82), m2 = self->io.read16(0x84), m3 = self->io.read16(0x86);
        os_log(OS_LOG_DEFAULT, "QLE2560: async event=%04x mb1=%04x mb2=%04x mb3=%04x", code, m1, m2,
               m3);
        if (code == 0x8002 || code == 0x8003 || code == 0x8004)
            self->fatal = true;
        if (code >= 0x8010 && code <= 0x8015)
            self->linkChanged = true;
        return true;
    }
    bool command(uint16_t (&input)[32], uint32_t timeout = 1000000) {
        const auto result = mailbox.execute(input, lastMailbox, timeout);
        if (result != isp25xx::MailboxResult::complete) {
            os_log(OS_LOG_DEFAULT,
                   "QLE2560: command=%04x transport=%u status=%04x detail=%04x %04x", input[0],
                   unsigned(result), lastMailbox[0], lastMailbox[1], lastMailbox[2]);
            if (result != isp25xx::MailboxResult::firmwareError)
                fatal = true;
            return false;
        }
        return !fatal;
    }
    bool flashRead(uint32_t wordAddress, uint32_t &data) {
        if (wordAddress >= 0x100000)
            return false;
        io.write32(0, 0x7ff00000 + wordAddress);
        const uint64_t start = io.nowMicros();
        do {
            const uint32_t address = io.read32(0);
            if (address == UINT32_MAX)
                return false;
            if (address & 0x80000000) {
                data = io.read32(4);
                return true;
            }
            io.waitMicros(10);
        } while (io.nowMicros() - start < 300000);
        return false;
    }
    bool readNvram() {
        // QLE2560 function zero. Validate the complete NVRAM checksum and WWNs;
        // never fabricate a worldwide name when board data is unavailable.
        uint32_t base = 0x48000;
        uint8_t layout[4096]{};
        uint32_t word = 0;
        if (!flashRead(0x50400, word))
            return false;
        isp25xx::put32(layout, word);
        const unsigned layoutLength = isp25xx::le16(layout + 2);
        if (isp25xx::le16(layout) == 1 && layoutLength && layoutLength % 16 == 0 &&
            layoutLength <= 4088) {
            for (unsigned i = 1; i < (layoutLength + 8) / 4; ++i) {
                if (!flashRead(0x50400 + i, word))
                    return false;
                isp25xx::put32(layout + i * 4, word);
            }
            uint16_t sum = 0;
            for (unsigned i = 0; i < layoutLength + 8; i += 2)
                sum += isp25xx::le16(layout + i);
            if (!sum)
                for (unsigned i = 8; i < layoutLength + 8; i += 16) {
                    if (isp25xx::le16(layout + i) != 0x14)
                        continue;
                    const uint32_t address = isp25xx::le32(layout + i + 8);
                    if (address % 4 || address / 4 > 0xfff00)
                        return false;
                    base = address / 4;
                }
        }
        for (unsigned i = 0; i < 128; ++i) {
            uint32_t value = 0;
            if (!flashRead((base | 0x80) + i, value))
                return false;
            isp25xx::put32(nvram + i * 4, value);
        }
        uint32_t checksum = 0;
        for (unsigned i = 0; i < 512; i += 4)
            checksum += isp25xx::le32(nvram + i);
        os_log(OS_LOG_DEFAULT,
               "QLE2560: NVRAM base=%x header=%08x version=%u ICB=%u checksum=%x options=%08x",
               base, isp25xx::le32(nvram), isp25xx::le16(nvram + 4), isp25xx::le16(nvram + 8),
               checksum, isp25xx::le32(nvram + 44));
        if (!isp25xx::validNvram(nvram)) {
            os_log(OS_LOG_DEFAULT, "QLE2560: NVRAM validation failed");
            return false;
        }
        const bool alt = (isp25xx::le32(nvram + 256) & 0x8000) != 0;
        os_log(OS_LOG_DEFAULT, "QLE2560: NVRAM valid WWPN=%016llx WWNN=%016llx",
               isp25xx::be64(nvram + (alt ? 260 : 20)), isp25xx::be64(nvram + (alt ? 268 : 28)));
        return true;
    }
    bool initialize() {
        isp25xx::FirmwareSegment segments[2];
        if (!isp25xx::parseFirmware(isp25xx::firmwareImage, sizeof(isp25xx::firmwareImage),
                                    segments))
            return false;
        if (!io.enableDMA(false))
            return false;
        const auto reset = isp25xx::resetController(io);
        os_log(OS_LOG_DEFAULT, "QLE2560: reset result=%u", unsigned(reset));
        if (reset != isp25xx::ResetResult::complete)
            return false;
        mailbox.resetSession();
        fatal = false;
        if (!readNvram())
            return false;
        auto r = scratch.create(io.pci, 65536);
        if (!r)
            r = requestRing.create(io.pci, queueDepth * 64);
        if (!r)
            r = responseRing.create(io.pci, queueDepth * 64);
        if (r) {
            os_log(OS_LOG_DEFAULT, "QLE2560: DMA allocation failed=%x", r);
            return false;
        }
        os_log(OS_LOG_DEFAULT,
               "QLE2560: DMA mappings ready scratch=%llx request=%llx response=%llx",
               scratch.address, requestRing.address, responseRing.address);
        if (!io.enableDMA(true))
            return false;
        for (const auto &segment : segments) {
            for (uint32_t cursor = 0; cursor < segment.words;) {
                const uint32_t count =
                    (segment.words - cursor > 16384) ? 16384 : segment.words - cursor;
                for (uint32_t i = 0; i < count; ++i)
                    isp25xx::put32(
                        scratch.bytes + i * 4,
                        isp25xx::be32(isp25xx::firmwareImage + segment.offset + (cursor + i) * 4));
                uint16_t m[32] = {0x0b};
                isp25xx::mailboxDMA(m, scratch.address);
                const uint32_t target = segment.address + cursor;
                m[1] = target;
                m[8] = target >> 16;
                m[4] = count >> 16;
                m[5] = count;
                if (!command(m, 5000000))
                    return false;
                cursor += count;
            }
            os_log(OS_LOG_DEFAULT, "QLE2560: firmware segment loaded address=%x words=%u",
                   segment.address, segment.words);
        }
        uint16_t verify[32] = {7};
        verify[1] = segments[0].address >> 16;
        verify[2] = segments[0].address;
        if (!command(verify, 5000000))
            return false;
        uint16_t execute[32] = {2};
        execute[1] = verify[1];
        execute[2] = verify[2];
        if (!command(execute, 5000000))
            return false;
        uint16_t version[32] = {8};
        if (!command(version))
            return false;
        os_log(OS_LOG_DEFAULT, "QLE2560: operational firmware=%u.%u.%u attributes=%04x",
               lastMailbox[1], lastMailbox[2], lastMailbox[3], lastMailbox[6]);
        uint8_t icb[128];
        if (!isp25xx::makeICB(icb, nvram, requestRing.address, responseRing.address, queueDepth))
            return false;
        memcpy(scratch.bytes, icb, sizeof(icb));
        for (uint64_t offset = 0x1c; offset <= 0x28; offset += 4)
            io.write32(offset, 0);
        uint16_t init[32] = {0x60};
        isp25xx::mailboxDMA(init, scratch.address);
        if (!command(init, 5000000))
            return false;
        initialized = true;
        requestIn = responseOut = 0;
        senseTask = -1;
        targetReady = false;
        os_log(OS_LOG_DEFAULT, "QLE2560: firmware queues initialized");
        return true;
    }
    bool discover() {
        targetReady = false;
        if (firmwareState != 3 || fatal)
            return false;
        // The supported installation is one directly connected target.
        if (topology != 0 && topology != 2) {
            os_log(OS_LOG_DEFAULT, "QLE2560: unsupported switched topology=%u", topology);
            return false;
        }
        memset(scratch.bytes, 0, 65536);
        uint16_t ids[32] = {0x7c};
        isp25xx::mailboxDMA(ids, scratch.address);
        if (!command(ids))
            return false;
        const unsigned count = lastMailbox[1];
        if (count > 2048) {
            fatal = true;
            return false;
        }
        uint16_t handles[2048];
        unsigned usable = 0;
        for (unsigned i = 0; i < count; ++i) {
            const uint16_t h = isp25xx::le16(scratch.bytes + i * 8 + 4);
            const auto *record = scratch.bytes + i * 8;
            const uint32_t remote =
                uint32_t(record[2]) << 16 | uint32_t(record[1]) << 8 | record[0];
            os_log(OS_LOG_DEFAULT, "QLE2560: ID list handle=%04x port=%06x", h, remote);
            // Adapter loop ID and remote N-port handle are different namespaces.
            if (h < 0x7f0 && remote != portID && (record[2] & 0xf0) != 0xf0)
                handles[usable++] = h;
        }
        os_log(OS_LOG_DEFAULT, "QLE2560: discovery id entries=%u candidates=%u", count, usable);
        for (unsigned i = 0; i < usable; ++i) {
            memset(scratch.bytes, 0, 64);
            uint16_t pdb[32] = {0x64};
            pdb[1] = handles[i];
            isp25xx::mailboxDMA(pdb, scratch.address);
            if (!command(pdb)) {
                if (fatal)
                    return false;
                continue;
            }
            const auto *p = scratch.bytes;
            const uint32_t port = uint32_t(p[8]) << 16 | uint32_t(p[9]) << 8 | p[10];
            os_log(OS_LOG_DEFAULT,
                   "QLE2560: port handle=%04x state=%u id=%06x WWPN=%016llx prli=%02x%02x",
                   handles[i], p[2] & 15, port, isp25xx::be64(p + 24), p[22], p[23]);
            if ((p[2] & 15) != 6 || !(p[22] & 0x10))
                continue; // qla24xx PDB's first service-parameter byte.
            if (targetReady) {
                os_log(OS_LOG_DEFAULT, "QLE2560: multiple targets rejected in direct mode");
                targetReady = false;
                return false;
            }
            targetHandle = handles[i];
            targetPort = port;
            targetWWPN = isp25xx::be64(p + 24);
            targetWWNN = isp25xx::be64(p + 32);
            targetReady = true;
        }
        return targetReady;
    }
    bool enqueue(const uint8_t (&entry)[64]) {
        const uint32_t out = io.read32(0x20);
        if (out >= queueDepth) {
            fatal = true;
            return false;
        }
        const uint16_t next = (requestIn + 1) % queueDepth;
        if (next == out)
            return false;
        memcpy(requestRing.bytes + requestIn * 64, entry, 64);
        std::atomic_thread_fence(std::memory_order_release);
        requestIn = next;
        io.write32(0x1c, requestIn);
        io.read32(0x1c);
        return true;
    }
    bool submit(unsigned slot, const uint8_t *lun, const uint8_t *cdb, uint8_t length,
                uint64_t address, uint32_t bytes, uint8_t direction, uint8_t attribute,
                uint32_t timeoutMs) {
        if (!initialized || fatal || !targetReady || slot >= taskCount || tasks[slot].active)
            return false;
        const uint32_t handle = begin(
            slot, bytes, timeoutMs ? io.nowMicros() + uint64_t(timeoutMs) * 1000 : UINT64_MAX);
        if (!handle)
            return false;
        auto &task = tasks[slot];
        uint8_t entry[64];
        if (!isp25xx::makeCommand(entry, handle, targetHandle, targetPort, lun, cdb, length,
                                  address, bytes, direction, attribute, timeoutMs)) {
            task.active = false;
            return false;
        }
        if (!enqueue(entry)) {
            task.active = false;
            return false;
        }
        return true;
    }
    void poll() {
        if (!initialized || fatal)
            return;
        for (unsigned event = 0; event < 32; ++event) {
            const uint32_t host = io.read32(isp25xx::hostStatus);
            if (host == UINT32_MAX || (host & isp25xx::riscPaused)) {
                os_log(OS_LOG_DEFAULT, "QLE2560: poll host fault=%08x", host);
                fatal = true;
                return;
            }
            if (!(host & isp25xx::riscInterrupt))
                break;
            if (!handleEvent(this, host)) {
                os_log(OS_LOG_DEFAULT, "QLE2560: unexpected interrupt=%08x", host);
                fatal = true;
                return;
            }
            io.write32(isp25xx::hostCommand, isp25xx::clearRiscInterrupt);
            io.read32(isp25xx::hostCommand);
        }
        const uint32_t in = io.read32(0x24);
        if (in >= queueDepth) {
            os_log(OS_LOG_DEFAULT, "QLE2560: invalid response producer=%08x", in);
            fatal = true;
            return;
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        for (unsigned count = 0; responseOut != in && count < queueDepth; ++count) {
            uint8_t entry[64];
            memcpy(entry, responseRing.bytes + responseOut * 64, 64);
            if (isp25xx::isPortIDReport(entry)) {
                linkChanged = true;
                os_log(OS_LOG_DEFAULT,
                       "QLE2560: port ID report format=%u status=%u port=%02x%02x%02x", entry[15],
                       entry[11], entry[14], entry[13], entry[12]);
            } else if (!consume(entry)) {
                os_log(OS_LOG_DEFAULT,
                       "QLE2560: invalid response index=%u header=%08x handle=%08x status=%08x",
                       responseOut, isp25xx::le32(entry), isp25xx::le32(entry + 4),
                       isp25xx::le32(entry + 8));
                fatal = true;
                return;
            }
            memset(responseRing.bytes + responseOut * 64, 0, 64);
            responseOut = (responseOut + 1) % queueDepth;
        }
        io.write32(0x28, responseOut);
        const uint64_t now = io.nowMicros();
        for (unsigned slot = 0; slot < taskCount; ++slot) {
            auto &task = tasks[slot];
            if (!task.active || task.done || now < task.deadline)
                continue;
            if (task.timeoutAbort) {
                os_log(OS_LOG_DEFAULT, "QLE2560: abort completion deadline exceeded slot=%u", slot);
                fatal = true;
                return;
            }
            // Abort the exchange before considering adapter-wide recovery. An
            // accepted abort is not proof that task DMA has stopped: retain the
            // request until its original completion arrives or reset quiesces it.
            if (!startTimeoutAbort(slot, now, 10000000))
                continue;
            os_log(OS_LOG_DEFAULT,
                   "QLE2560: deadline expired slot=%u handle=%08x; issuing ABORT IOCB", slot,
                   task.completion.handle);
            if (!abort(slot)) {
                os_log(OS_LOG_DEFAULT, "QLE2560: ABORT IOCB failed slot=%u; falling back to reset",
                       slot);
                fatal = true;
                return;
            }
            os_log(OS_LOG_DEFAULT,
                   "QLE2560: ABORT IOCB accepted slot=%u; awaiting original completion", slot);
            return; // Drain any responses produced by the mailbox on next tick.
        }
    }
    bool issueIOCB(uint8_t (&entry)[64], uint32_t timeout = 30000000) {
        if (!initialized || fatal)
            return false;
        memcpy(scratch.bytes, entry, 64);
        uint16_t m[32] = {0x54};
        isp25xx::mailboxDMA(m, scratch.address);
        if (!command(m, timeout))
            return false;
        std::atomic_thread_fence(std::memory_order_acquire);
        memcpy(entry, scratch.bytes, 64);
        return !(entry[3] & 0x3c) && isp25xx::le16(entry + 8) == 0;
    }
    bool abort(unsigned slot) {
        if (slot >= taskCount || !tasks[slot].active)
            return true;
        uint8_t entry[64]{};
        entry[0] = 0x33;
        entry[1] = 1;
        isp25xx::put16(entry + 8, targetHandle);
        isp25xx::put32(entry + 12, tasks[slot].completion.handle);
        entry[48] = targetPort;
        entry[49] = targetPort >> 8;
        entry[50] = targetPort >> 16;
        return issueIOCB(entry, 3000000);
    }
    bool taskManagement(uint64_t lun, uint32_t flags) {
        if (!targetReady || lun > 0x3fff)
            return false;
        uint8_t entry[64]{};
        entry[0] = 0x14;
        entry[1] = 1;
        isp25xx::put16(entry + 8, targetHandle);
        isp25xx::put16(entry + 14, 20);
        entry[16] = lun > 255 ? uint8_t(0x40 | (lun >> 8)) : 0;
        entry[17] = lun;
        isp25xx::swapWords(entry + 16, 8);
        isp25xx::put32(entry + 24, flags);
        entry[48] = targetPort;
        entry[49] = targetPort >> 8;
        entry[50] = targetPort >> 16;
        if (!issueIOCB(entry))
            return false;
        isp25xx::Completion result;
        if (!isp25xx::decodeStatus(entry, 0, result) ||
            result.result != isp25xx::IOResult::complete)
            return false;
        uint8_t marker[64]{};
        marker[0] = 4;
        marker[1] = 1;
        isp25xx::put16(marker + 8, targetHandle);
        marker[10] = (flags == 2) ? 1 : 0;
        marker[16] = lun > 255 ? uint8_t(0x40 | (lun >> 8)) : 0;
        marker[17] = lun;
        isp25xx::swapWords(marker + 16, 8);
        return enqueue(marker);
    }
    bool queryLink() {
        uint16_t state[32] = {0x69};
        if (!command(state))
            return false;
        firmwareState = lastMailbox[1];
        if (firmwareState != 3)
            return true;
        uint16_t id[32] = {0x20};
        if (!command(id))
            return false;
        ownHandle = lastMailbox[1];
        portID = lastMailbox[2] | uint32_t(lastMailbox[3] & 0xff) << 16;
        topology = lastMailbox[6];
        os_log(OS_LOG_DEFAULT, "QLE2560: firmware ready topology=%u localPort=%06x handle=%04x",
               topology, portID, ownHandle);
        return true;
    }
    bool shutdown(bool sessionClosed = false) {
        initialized = false;
        // An all-ones MMIO read is an access error, not proof DMA has stopped.
        // PCI Close disables bus mastering and is the fallback for failed reset.
        bool safe = sessionClosed;
        if (!safe) {
            const auto result = isp25xx::resetController(io);
            safe = result == isp25xx::ResetResult::complete;
            io.enableDMA(false);
            os_log(OS_LOG_DEFAULT, "QLE2560: shutdown reset=%u", unsigned(result));
        }
        if (safe) {
            responseRing.release();
            requestRing.release();
            scratch.release();
        } else
            os_log(OS_LOG_DEFAULT,
                   "QLE2560: unable to quiesce DMA; retaining mappings until device removal");
        return safe;
    }
};
