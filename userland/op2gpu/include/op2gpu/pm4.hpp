// Adreno 4xx command-processor packets: PM4 type 0 (register writes) and type 3 (opcodes).
//
// Only what has been measured on the phone is here (logs/daily-20261111-0446*.log, -0503*.log):
// CP_MEM_WRITE (any length up to the 14-bit count), CP_MEM_TO_MEM 32 and 64 bit, CP_WAIT_FOR_IDLE, CP_NOP,
// type-0 writes. Not here, on purpose:
//   CP_EVENT_WRITE(CACHE_FLUSH_TS): never retires with the 3D pipe uninitialised, and no memory write after it lands.
//   CP_MEMCPY: A6xx and later.
//   CP_MEM_TO_REG/REG_TO_MEM with a count: the count encoding tried hung the CP (AHB write to a wrong register).
#pragma once

#include <cstdint>
#include <vector>

namespace op2gpu::pm4 {

enum Opcode : uint8_t {
    kNop = 0x10,
    kWaitForIdle = 0x26,
    kMemWrite = 0x3d,
    kIndirectBuffer = 0x3f,   // CP_INDIRECT_BUFFER_PFE (the kernel's ring uses it; a stream does not)
    kMemToMem = 0x73,
};

constexpr uint32_t kType3 = 3u << 30;
constexpr uint32_t kMaxType3Count = 0x3fff;   // 14-bit count field (count - 1 is stored)

// header of a type-3 packet with COUNT payload words (1..0x4000)
constexpr uint32_t type3(uint8_t op, uint32_t count) { return kType3 | ((count - 1) << 16) | (uint32_t(op) << 8); }
// header of a type-0 packet writing COUNT consecutive registers from REG (dword index)
constexpr uint32_t type0(uint32_t reg, uint32_t count) { return ((count - 1) << 16) | (reg & 0x7fff); }

constexpr uint32_t kMemToMemDouble = 1u << 29;   // CP_MEM_TO_MEM: 64-bit values

// A command stream being written. Addresses are 32-bit on A4xx (the GPU address is the physical one).
class Stream {
public:
    void nop(uint32_t marker) { w_.push_back(type3(kNop, 1)); w_.push_back(marker); }
    void waitForIdle() { w_.push_back(type3(kWaitForIdle, 1)); w_.push_back(0); }
    // WORDS words (1..0x3ffe) written at ADDRESS
    void memWrite(uint32_t address, const uint32_t* words, uint32_t count) {
        w_.push_back(type3(kMemWrite, count + 1));
        w_.push_back(address);
        w_.insert(w_.end(), words, words + count);
    }
    // one value copied: 64-bit when DOUBLE (both addresses 8-aligned), else 32-bit
    void memToMem(uint32_t destination, uint32_t source, bool doubleWord) {
        w_.push_back(type3(kMemToMem, 3));
        w_.push_back(doubleWord ? kMemToMemDouble : 0);
        w_.push_back(destination);
        w_.push_back(source);
    }
    void reg(uint32_t index, uint32_t value) { w_.push_back(type0(index, 1)); w_.push_back(value); }

    const std::vector<uint32_t>& words() const { return w_; }
    size_t size() const { return w_.size(); }
    void clear() { w_.clear(); }

private:
    std::vector<uint32_t> w_;
};

}  // namespace op2gpu::pm4
