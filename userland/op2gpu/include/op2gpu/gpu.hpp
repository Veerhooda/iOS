// The OnePlus 2's Adreno 430 through the kernel's OP2GPU user client (iokit/Kernel/arm/OP2GPU.cpp).
//
// The requests are Linux msm-DRM's, by number, with their structures as they are; the kernel answers them as
// struct-method calls whose selector is the request number. Adapted from hakios-metal's gpu.hpp
// (MIT, Copyright (c) 2025-2026 the HakiOS authors; see LICENSE-hakios).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace op2gpu {

enum Request : uint32_t {
    kGemClose = 0x09,
    kGetParam = 0x40,
    kGemNew = 0x42,
    kGemInfo = 0x43,
    kGemCpuPrep = 0x44,
    kGemCpuFini = 0x45,
    kGemSubmit = 0x46,
    kWaitFence = 0x47,
    kSubmitQueueNew = 0x4a,
    kSubmitQueueClose = 0x4b,
    kLog = 0x7c,   // OP2GPU's own: a line of text into the kernel log (what the phone's ramoops keeps)
};

struct MsmTimespec { int64_t seconds, nanoseconds; };
struct MsmParam { uint32_t pipe, param; uint64_t value; uint32_t len, pad; };
struct MsmGemNew { uint64_t size; uint32_t flags, handle; };
struct MsmGemInfo { uint32_t handle, info; uint64_t value; uint32_t len, pad; };
struct MsmGemCpuPrep { uint32_t handle, op; MsmTimespec timeout; };
struct MsmGemCpuFini { uint32_t handle; };
struct MsmSubmitCmd { uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs; uint64_t relocs; };
struct MsmSubmitBo { uint32_t flags, handle; uint64_t presumed; };
struct MsmGemSubmit {
    uint32_t flags, fence, nr_bos, nr_cmds;
    uint64_t bos, cmds;
    int32_t fence_fd;
    uint32_t queueid;
    uint64_t in_syncobjs, out_syncobjs;
    uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};
struct MsmWaitFence { uint32_t fence, flags; MsmTimespec timeout; uint32_t queueid; };
struct MsmSubmitQueue { uint32_t flags, prio, id; };
struct DrmGemClose { uint32_t handle, pad; };

enum : uint32_t {
    kPipe3D = 1,
    kParamGpuId = 1,
    kParamGmemSize = 2,
    kParamChipId = 3,
    kParamMaxFreq = 4,
    kParamTimestamp = 5,
    kParamFaults = 9,   // the kernel's GPU resets after hangs (OP2GPU fHangs)
    kInfoGetIova = 1,
    kSubmitBoRead = 0x1,
    kSubmitBoWrite = 0x2,
    kSubmitCmdBuf = 1,
};

constexpr uint32_t kChipA430 = 0x04030002;

// --- the one part that talks to the kernel ------------------------------------------------------------------------

class Transport {
public:
    virtual ~Transport() = default;
    // the request with its structure in and out; 0, or an errno
    virtual int call(uint32_t request, void* data, size_t size) = 0;
    // the buffer's memory in this process (write-combined on the phone); nullptr if not
    virtual void* map(uint32_t handle, uint64_t size) = 0;
    virtual void unmap(uint32_t handle, void* address) = 0;
    // now, in ns of the clock the kernel's deadlines are in (mach_continuous_time on the phone)
    virtual int64_t nowNs() = 0;
};

// OP2GPU's user client (Darwin only, src/iokit_transport.cpp); nullptr and *why when there is none
std::unique_ptr<Transport> openKernelGpu(std::string* why);

// --- the GPU --------------------------------------------------------------------------------------------------------

class Gpu;

// One GEM object: physically contiguous, its GPU address is its physical address (the SMMU is in bypass).
class Bo {
public:
    ~Bo();
    Bo(const Bo&) = delete;
    Bo& operator=(const Bo&) = delete;

    uint32_t handle() const { return handle_; }
    uint64_t size() const { return size_; }
    uint64_t gpuAddress() const { return iova_; }
    void* cpu() const { return cpu_; }   // write-combined: write it in order, read it rarely

private:
    friend class Gpu;
    Bo(Gpu& gpu, uint32_t handle, uint64_t size, uint64_t iova, void* cpu)
        : gpu_(gpu), handle_(handle), size_(size), iova_(iova), cpu_(cpu) {}
    Gpu& gpu_;
    uint32_t handle_;
    uint64_t size_;
    uint64_t iova_;
    void* cpu_;
};

struct BoUse {
    const Bo* bo;
    bool write;
};

class Gpu {
public:
    explicit Gpu(std::unique_ptr<Transport> transport) : t_(std::move(transport)) {}

    int param(uint32_t which, uint64_t* value);
    uint32_t chipId();
    uint64_t faults();

    // SIZE bytes rounded up to 16 KB; nullptr with *error set
    std::unique_ptr<Bo> newBo(uint64_t size, uint32_t flags, int* error);
    uint64_t allocatedBytes() const { return allocated_.load(std::memory_order_relaxed); }

    int newQueue(uint32_t priority, uint32_t* id);
    void closeQueue(uint32_t id);

    // words [offset, offset + words) of COMMANDS run as one indirect buffer, with the buffers it reads and writes
    int submit(uint32_t queue, const Bo& commands, uint32_t offsetBytes, uint32_t words, const std::vector<BoUse>& uses,
               uint32_t* fence);
    // 0, ETIMEDOUT, or another errno
    int wait(uint32_t queue, uint32_t fence, int64_t timeoutNs);
    int cpuPrep(const Bo& bo, uint32_t op, int64_t timeoutNs);
    void cpuFini(const Bo& bo);

    // a line into the kernel log (the phone's only log that survives the reset into TWRP)
    void log(const std::string& line);

    Transport& transport() { return *t_; }

private:
    friend class Bo;
    void release(Bo& bo);
    std::atomic<uint64_t> allocated_{0};
    std::unique_ptr<Transport> t_;
};

}  // namespace op2gpu
