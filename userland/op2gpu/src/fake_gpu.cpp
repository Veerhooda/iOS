#include "op2gpu/fake_gpu.hpp"

#include <cerrno>
#include <chrono>
#include <cstring>

#include "op2gpu/pm4.hpp"

namespace op2gpu {

namespace {

class FakeGpu : public Transport {
public:
    explicit FakeGpu(std::shared_ptr<FakeGpuState> s) : s_(std::move(s)) {}

    int call(uint32_t request, void* data, size_t size) override {
        switch (request) {
        case kGetParam: {
            if (size < sizeof(MsmParam)) return EINVAL;
            auto* p = static_cast<MsmParam*>(data);
            switch (p->param) {
            case kParamGpuId:
            case kParamChipId: p->value = kChipA430; return 0;
            case kParamGmemSize: p->value = 0x180000; return 0;
            case kParamFaults: p->value = s_->faults; return 0;
            default: return EINVAL;
            }
        }
        case kGemNew: {
            if (size < sizeof(MsmGemNew)) return EINVAL;
            auto* n = static_cast<MsmGemNew*>(data);
            if (n->size == 0 || n->size > (64ull << 20)) return EINVAL;
            uint64_t sz = (n->size + 4095) & ~4095ull;
            FakeGpuState::Buffer b;
            b.iova = s_->nextIova;
            b.bytes.assign(sz, 0);
            s_->nextIova += sz + 0x10000;   // a gap: a write past the end hits no buffer
            n->handle = s_->nextHandle++;
            s_->buffers[n->handle] = std::move(b);
            return 0;
        }
        case kGemInfo: {
            auto* i = static_cast<MsmGemInfo*>(data);
            auto it = s_->buffers.find(i->handle);
            if (it == s_->buffers.end()) return ENOENT;
            i->value = it->second.iova;
            return 0;
        }
        case kGemClose: {
            auto* c = static_cast<DrmGemClose*>(data);
            return s_->buffers.erase(c->handle) ? 0 : ENOENT;
        }
        case kSubmitQueueNew: static_cast<MsmSubmitQueue*>(data)->id = 0; return 0;
        case kSubmitQueueClose: return 0;
        case kGemCpuPrep:
        case kGemCpuFini: return 0;
        case kLog: s_->log.emplace_back(static_cast<const char*>(data), size); return 0;
        case kGemSubmit: return submit(*static_cast<MsmGemSubmit*>(data));
        case kWaitFence: {
            auto* w = static_cast<MsmWaitFence*>(data);
            if (int32_t(s_->fenceDone - w->fence) >= 0) return 0;
            // a hung submission: the "kernel" resets and retires it, as OP2GPU::recover does
            s_->faults++;
            s_->fenceDone = s_->fenceSeq;
            return ETIMEDOUT;
        }
        default:
            return ENOSYS;
        }
    }

    void* map(uint32_t handle, uint64_t) override {
        auto it = s_->buffers.find(handle);
        return it == s_->buffers.end() ? nullptr : it->second.bytes.data();
    }
    void unmap(uint32_t, void*) override {}
    int64_t nowNs() override {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch()).count();
    }

private:
    // the bytes [address, address + n) inside one buffer, or nullptr
    uint8_t* at(uint64_t address, uint64_t n) {
        for (auto& kv : s_->buffers) {
            FakeGpuState::Buffer& b = kv.second;
            if (address >= b.iova && address + n <= b.iova + b.bytes.size()) return b.bytes.data() + (address - b.iova);
        }
        return nullptr;
    }

    bool fault(const std::string& why) {
        s_->log.push_back("FAULT: " + why);
        return false;
    }

    // run WORDS as the CP would; false on a fault (the fence then never passes)
    bool run(const uint32_t* w, uint32_t n) {
        uint32_t i = 0;
        while (i < n) {
            uint32_t h = w[i];
            uint32_t type = h >> 30;
            uint32_t count = ((h >> 16) & 0x3fff) + 1;
            if (i + 1 + count > n) return fault("packet at word " + std::to_string(i) + " runs past the end");
            const uint32_t* p = w + i + 1;
            s_->packets++;
            if (type == 0) {
                for (uint32_t k = 0; k < count; k++) s_->registers[(h & 0x7fff) + k] = p[k];
            } else if (type == 3) {
                uint8_t op = (h >> 8) & 0xff;
                switch (op) {
                case pm4::kNop:
                case pm4::kWaitForIdle:
                    break;
                case pm4::kMemWrite: {
                    if (count < 2) return fault("CP_MEM_WRITE without data");
                    uint8_t* d = at(p[0], 4ull * (count - 1));
                    if (!d) return fault("CP_MEM_WRITE outside every buffer");
                    memcpy(d, p + 1, 4ull * (count - 1));
                    break;
                }
                case pm4::kMemToMem: {
                    if (count != 3) return fault("CP_MEM_TO_MEM of " + std::to_string(count) + " words (want 3)");
                    if (p[0] & ~pm4::kMemToMemDouble) return fault("CP_MEM_TO_MEM flags not measured on the phone");
                    uint32_t bytes = (p[0] & pm4::kMemToMemDouble) ? 8 : 4;
                    if ((bytes == 8) && ((p[1] | p[2]) & 7)) return fault("64-bit CP_MEM_TO_MEM not 8-aligned");
                    uint8_t* d = at(p[1], bytes);
                    uint8_t* s = at(p[2], bytes);
                    if (!d || !s) return fault("CP_MEM_TO_MEM outside every buffer");
                    memmove(d, s, bytes);
                    break;
                }
                default:
                    return fault("opcode 0x" + hex(op) + " is not one measured on the A430");
                }
            } else {
                return fault("packet type " + std::to_string(type));
            }
            i += 1 + count;
        }
        return true;
    }

    static std::string hex(uint32_t v) {
        char b[16];
        snprintf(b, sizeof b, "%x", v);
        return b;
    }

    int submit(MsmGemSubmit& s) {
        if (s.nr_cmds == 0 || s.nr_bos == 0) return EINVAL;
        auto* bos = reinterpret_cast<const MsmSubmitBo*>(uintptr_t(s.bos));
        auto* cmds = reinterpret_cast<const MsmSubmitCmd*>(uintptr_t(s.cmds));
        for (uint32_t b = 0; b < s.nr_bos; b++) {
            if (!s_->buffers.count(bos[b].handle)) return ENOENT;
        }
        bool ok = true;
        for (uint32_t c = 0; c < s.nr_cmds && ok; c++) {
            if (cmds[c].submit_idx >= s.nr_bos) return EINVAL;
            FakeGpuState::Buffer& b = s_->buffers[bos[cmds[c].submit_idx].handle];
            if (cmds[c].size % 4 || cmds[c].submit_offset % 4 || cmds[c].submit_offset + uint64_t(cmds[c].size) > b.bytes.size())
                return EINVAL;
            std::vector<uint32_t> w(cmds[c].size / 4);
            memcpy(w.data(), b.bytes.data() + cmds[c].submit_offset, cmds[c].size);
            ok = run(w.data(), uint32_t(w.size()));
        }
        s.fence = ++s_->fenceSeq;
        if (ok) s_->fenceDone = s.fence;
        return 0;
    }

    std::shared_ptr<FakeGpuState> s_;
};

}  // namespace

std::unique_ptr<Transport> newFakeGpu(std::shared_ptr<FakeGpuState> state) {
    return std::unique_ptr<Transport>(new FakeGpu(std::move(state)));
}

}  // namespace op2gpu
