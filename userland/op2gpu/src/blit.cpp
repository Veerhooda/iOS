#include "op2gpu/blit.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>

namespace op2gpu {

namespace {
bool inside(const Bo& b, uint64_t offset, uint64_t length) {
    return offset <= b.size() && length <= b.size() - offset;
}
bool words(uint64_t v) { return v % 4 == 0; }
}  // namespace

void Batch::use(const Bo& b, bool write) {
    for (BoUse& u : uses_) {
        if (u.bo == &b) {
            u.write = u.write || write;
            return;
        }
    }
    uses_.push_back(BoUse{&b, write});
}

bool Batch::fill(const Bo& b, uint64_t offset, uint64_t length, uint8_t value, std::string* why) {
    if (!words(offset) || !words(length)) {
        *why = "fill of bytes that are not whole words (offset " + std::to_string(offset) + ", length " +
               std::to_string(length) + "): the CP writes words; this needs the 2D engine";
        return false;
    }
    if (!inside(b, offset, length)) {
        *why = "fill outside the buffer";
        return false;
    }
    if (length == 0) return true;
    uint32_t v = uint32_t(value) * 0x01010101u;
    std::vector<uint32_t> chunk(std::min<uint64_t>(length / 4, kFillChunk), v);
    uint64_t done = 0, total = length / 4;
    while (done < total) {
        uint32_t n = uint32_t(std::min<uint64_t>(total - done, kFillChunk));
        s_.memWrite(uint32_t(b.gpuAddress() + offset + 4 * done), chunk.data(), n);
        done += n;
    }
    // what comes after may read what this wrote
    s_.waitForIdle();
    use(b, true);
    return true;
}

bool Batch::copy(const Bo& from, uint64_t fromOffset, const Bo& to, uint64_t toOffset, uint64_t length, std::string* why) {
    if (!words(fromOffset) || !words(toOffset) || !words(length)) {
        *why = "copy of bytes that are not whole words: the CP moves words; this needs the 2D engine";
        return false;
    }
    if (!inside(from, fromOffset, length) || !inside(to, toOffset, length)) {
        *why = "copy outside a buffer";
        return false;
    }
    if (length == 0) return true;
    uint64_t src = from.gpuAddress() + fromOffset, dst = to.gpuAddress() + toOffset;
    if (src < dst + length && dst < src + length) {
        *why = "copy between overlapping ranges";
        return false;
    }
    uint64_t total = length / 4, i = 0;
    // CP_MEM_TO_MEM: one value per packet; 64-bit where both sides allow it
    bool pairable = ((src ^ dst) & 7) == 0;
    if (pairable && (src & 7) != 0) {   // align to 8 with one 32-bit copy first
        s_.memToMem(uint32_t(dst), uint32_t(src), false);
        i = 1;
    }
    for (; i < total; ) {
        if (pairable && total - i >= 2) {
            s_.memToMem(uint32_t(dst + 4 * i), uint32_t(src + 4 * i), true);
            i += 2;
        } else {
            s_.memToMem(uint32_t(dst + 4 * i), uint32_t(src + 4 * i), false);
            i += 1;
        }
    }
    s_.waitForIdle();
    use(from, false);
    use(to, true);
    return true;
}

bool Batch::mark(const Bo& b, uint64_t offset, uint32_t value, std::string* why) {
    if (!words(offset) || !inside(b, offset, 4)) {
        *why = "mark outside the buffer or not on a word";
        return false;
    }
    s_.memWrite(uint32_t(b.gpuAddress() + offset), &value, 1);
    s_.waitForIdle();
    use(b, true);
    return true;
}

int Batch::run(uint32_t queue, int64_t timeoutNs, std::string* why) {
    if (s_.size() == 0) return 0;
    const std::vector<uint32_t>& w = s_.words();
    int e = 0;
    std::unique_ptr<Bo> commands = gpu_.newBo(4 * w.size(), 0, &e);
    if (!commands) {
        *why = "no memory for the commands (" + std::to_string(4 * w.size()) + " bytes): errno " + std::to_string(e);
        s_.clear();
        uses_.clear();
        return e ? e : ENOMEM;
    }
    memcpy(commands->cpu(), w.data(), 4 * w.size());
    uint64_t faults0 = gpu_.faults();
    uint32_t fence = 0;
    int64_t t0 = gpu_.transport().nowNs();
    e = gpu_.submit(queue, *commands, 0, uint32_t(w.size()), uses_, &fence);
    if (e == 0) e = gpu_.wait(queue, fence, timeoutNs);
    lastNs_ = gpu_.transport().nowNs() - t0;
    s_.clear();
    uses_.clear();
    if (e != 0) {
        *why = (e == ETIMEDOUT ? "the GPU did not finish in time" : "submit/wait failed") + std::string(": errno ") +
               std::to_string(e);
        return e;
    }
    // a fence the kernel retired by resetting a hung GPU: the work did not run
    if (gpu_.faults() != faults0) {
        *why = "the kernel reset the GPU during this batch";
        return EIO;
    }
    return 0;
}

}  // namespace op2gpu
