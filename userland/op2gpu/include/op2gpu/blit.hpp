// Blits done by the command processor alone (G1): fills and copies of words, recorded into a batch and run as one
// indirect buffer. What the CP cannot do (bytes that are not whole words) is refused by name: that is the 2D/3D
// engine's work, later.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "op2gpu/gpu.hpp"
#include "op2gpu/pm4.hpp"

namespace op2gpu {

class Batch {
public:
    explicit Batch(Gpu& gpu) : gpu_(gpu) {}

    // LENGTH bytes of B from OFFSET set to VALUE; offset and length whole words
    bool fill(const Bo& b, uint64_t offset, uint64_t length, uint8_t value, std::string* why);
    // LENGTH bytes copied; offsets and length whole words, the ranges may not overlap
    bool copy(const Bo& from, uint64_t fromOffset, const Bo& to, uint64_t toOffset, uint64_t length, std::string* why);
    // a word written at OFFSET: a marker the CPU can check (ordering tests)
    bool mark(const Bo& b, uint64_t offset, uint32_t value, std::string* why);

    // submit, wait up to TIMEOUT_NS, check the kernel did not reset the GPU meanwhile; the batch is empty afterwards.
    // 0, or an errno (ETIMEDOUT, EIO = the GPU was reset, so the batch did not run) with *why
    int run(uint32_t queue, int64_t timeoutNs, std::string* why);
    // how long run() waited for the GPU, ns
    int64_t lastRunNs() const { return lastNs_; }

    const pm4::Stream& stream() const { return s_; }
    bool empty() const { return s_.size() == 0; }

    // CP_MEM_WRITE payload per packet (the count field allows 0x3ffe; 16000 measured on the phone)
    static constexpr uint32_t kFillChunk = 16000;

private:
    void use(const Bo& b, bool write);
    Gpu& gpu_;
    pm4::Stream s_;
    std::vector<BoUse> uses_;
    int64_t lastNs_ = 0;
};

}  // namespace op2gpu
