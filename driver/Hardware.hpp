// SPDX-License-Identifier: GPL-2.0-only
#pragma once
#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IODMACommand.h>
#include <PCIDriverKit/IOPCIDevice.h>
#include <time.h>
#include <string.h>
#include <atomic>
#include <os/log.h>

struct PCIRegisters {
    IOPCIDevice *pci = nullptr;
    uint8_t index = 0;
    uint32_t read32(uint64_t offset) {
        uint32_t value = UINT32_MAX;
        pci->MemoryRead32(index, offset, &value);
        return value;
    }
    uint16_t read16(uint64_t offset) {
        uint16_t value = UINT16_MAX;
        pci->MemoryRead16(index, offset, &value);
        return value;
    }
    void write32(uint64_t offset, uint32_t value) {
        std::atomic_thread_fence(std::memory_order_seq_cst);
        pci->MemoryWrite32(index, offset, value);
    }
    void write16(uint64_t offset, uint16_t value) {
        pci->MemoryWrite16(index, offset, value);
    }
    uint64_t nowMicros() {
        return clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 1000;
    }
    void waitMicros(uint32_t duration) {
        if (duration >= 1000)
            IOSleep((duration + 999) / 1000);
        else
            IODelay(duration);
    }
    bool enableDMA(bool enable) {
        uint16_t command = UINT16_MAX;
        pci->ConfigurationRead16(4, &command);
        if (command == UINT16_MAX)
            return false;
        pci->ConfigurationWrite16(4, uint16_t(enable ? (command | 6) : ((command | 2) & ~4)));
        pci->ConfigurationRead16(4, &command);
        return command != UINT16_MAX && (command & 6) == (enable ? 6 : 2);
    }
};

// No automatic destructor: the controller explicitly releases DMA mappings
// only after shutdown proves DMA is stopped, or the PCI device is gone.
struct DMABuffer {
    IOBufferMemoryDescriptor *memory = nullptr;
    IODMACommand *mapping = nullptr;
    uint8_t *bytes = nullptr;
    uint64_t address = 0, size = 0;
    bool prepared = false;
    kern_return_t create(IOPCIDevice *pci, uint64_t length) {
        size = length;
        auto result =
            IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut, length, 4096, &memory);
        if (result)
            return result;
        IOAddressSegment range{};
        result = memory->GetAddressRange(&range);
        if (result || range.length < length) {
            release();
            return result ? result : kIOReturnNoMemory;
        }
        bytes = reinterpret_cast<uint8_t *>(range.address);
        memset(bytes, 0, length);
        IODMACommandSpecification specification{};
        specification.maxAddressBits = 64;
        result = IODMACommand::Create(pci, 0, &specification, &mapping);
        if (result) {
            release();
            return result;
        }
        IOAddressSegment segments[32]{};
        uint32_t count = 32;
        uint64_t flags = 0;
        result = mapping->PrepareForDMA(0, memory, 0, length, &flags, &count, segments);
        if (result) {
            release();
            return result;
        }
        prepared = true;
        if (count != 1 || segments[0].length < length || !segments[0].address ||
            segments[0].address % 64 || (flags & 3) != 3) {
            release();
            return kIOReturnUnsupported;
        }
        address = segments[0].address;
        return kIOReturnSuccess;
    }
    void release() {
        if (prepared) {
            mapping->CompleteDMA(0);
            prepared = false;
        }
        if (mapping) {
            mapping->release();
            mapping = nullptr;
        }
        if (memory) {
            memory->release();
            memory = nullptr;
        }
        bytes = nullptr;
        address = 0;
        size = 0;
    }
};
