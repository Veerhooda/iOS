/*
 * Kernel Adreno 430 GPU Driver for OnePlus 2 (MSM8994).
 *
 * Implements hardware bring-up for Qualcomm Adreno 430:
 * 1. MMSSCC clock branches & GDSC power domain uncollapse
 * 2. Adreno 430 MMIO mapping (0xfdb00000, 256 KB)
 * 3. Ringbuffer allocation (32 KB contiguous wired physical memory)
 * 4. Microcode firmware bootstrapping (a420_pm4.fw, a420_pfp.fw)
 * 5. Hardware command processing & PM4 Type 3 packet execution
 */

#ifndef _OP2_GPU_H
#define _OP2_GPU_H

#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_MSM8994)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>

/* Physical Base Addresses */
#define OP2_MMSSCC_PHYS_BASE            0xfd8c0000ULL
#define OP2_MMSSCC_PHYS_SIZE            0x6000ULL

#define OP2_ADRENO_PHYS_BASE            0xfdb00000ULL
#define OP2_ADRENO_PHYS_SIZE            0x40000ULL      /* 256 KB */

/* MMSSCC Register Offsets (Byte offsets from 0xfd8c0000) */
#define MMSS_OXILI_GX_GDSC              0x4024
#define MMSS_OXILI_CX_GDSC              0x4034
#define MMSS_OXILI_GFX3D_CBCR           0x4028
#define MMSS_OXILICX_AHB_CBCR           0x403c
#define MMSS_OXILI_RBBMTIMER_CBCR       0x40b0

/* GDSC Bits */
#define GDSC_SW_COLLAPSE_BIT            (1U << 0)
#define GDSC_PWR_ON_BIT                 (1U << 31)

/* CBCR Bits */
#define CBCR_CLK_ENABLE_BIT             (1U << 0)
#define CBCR_CLK_OFF_BIT                (1U << 31)

/* Adreno 4xx Word Offsets (Byte Offset = Word * 4) */
#define A4XX_RBBM_SW_RESET_CMD          0x022
#define A4XX_RBBM_AHB_CTL0              0x023
#define A4XX_RBBM_AHB_CTL1              0x024
#define A4XX_RBBM_WAIT_IDLE_CLOCKS_CTL  0x02b
#define A4XX_RBBM_RBBM_CTL              0x03e
#define A4XX_RBBM_PERFCTR_CTL           0x170
#define A4XX_RBBM_GPU_BUSY_MASKED       0x17a
#define A4XX_RBBM_STATUS                0x191
#define A4XX_RBBM_POWER_STATUS          0x1b0
#define A4XX_RBBM_WAIT_IDLE_CLOCKS_CTL2 0x1b8
#define A4XX_RBBM_INT_0_STATUS          0x17d
#define A4XX_RBBM_AHB_ERROR_STATUS      0x18f
#define A4XX_CP_ME_STATUS               0x4d1
#define A4XX_CP_HW_FAULT                0x4d8
#define A4XX_CP_PROTECT_STATUS          0x4da
#define A4XX_RBBM_SP_HYST_CNT           0x021
#define A4XX_RBBM_AHB_CMD               0x025
#define A4XX_RBBM_INTERFACE_HANG_INT_CTL 0x02f
#define A4XX_RBBM_INT_CLEAR_CMD         0x036
#define A4XX_RBBM_INT_0_MASK            0x037
#define A4XX_RBBM_POWER_CNTL_IP         0x098
#define A4XX_RBBM_SP_REGFILE_SLEEP_CNTL_0 0x099
#define A4XX_RBBM_SP_REGFILE_SLEEP_CNTL_1 0x09a
#define A4XX_RBBM_PERFCTR_RBBM_0_LO     0x0ac
#define A4XX_RBBM_PERFCTR_RBBM_0_HI     0x0ad
#define A4XX_RBBM_ALWAYSON_COUNTER_LO   0x16e
#define A4XX_RBBM_ALWAYSON_COUNTER_HI   0x16f
#define A4XX_RBBM_PERFCTR_RBBM_SEL_0    0x176
#define A4XX_SP_TP_PWR_ON               (1U << 20)

/* GPU SMMU (kgsl_iommu @ 0xfdb10000) = Adreno base + 0x10000 bytes */
#define OP2_GPU_SMMU_OFFSET             0x10000U

/* VBIF Registers (Word Offsets) */
#define A4XX_VBIF_GATE_OFF_WRREQ_EN     0x302a
#define A4XX_VBIF_IN_RD_LIM_CONF0       0x302c
#define A4XX_VBIF_IN_RD_LIM_CONF1       0x302d
#define A4XX_VBIF_IN_WR_LIM_CONF0       0x3030
#define A4XX_VBIF_IN_WR_LIM_CONF1       0x3031
#define A4XX_VBIF_ROUND_ROBIN_QOS_ARB   0x3049
#define A4XX_VBIF_XIN_HALT_CTRL0        0x3080
#define A4XX_VBIF_XIN_HALT_CTRL1        0x3081

#define A4XX_UCHE_CACHE_WAYS_VFD        0xe8c
#define A4XX_UCHE_TRAP_BASE_LO          0xe83
#define A4XX_UCHE_TRAP_BASE_HI          0xe84

#define A4XX_RB_GMEM_BASE_ADDR          0xcc0

#define A4XX_CP_RB_BASE                 0x200
#define A4XX_CP_RB_CNTL                 0x201
#define A4XX_CP_RB_RPTR_ADDR            0x203
#define A4XX_CP_RB_RPTR                 0x204
#define A4XX_CP_RB_WPTR                 0x205
#define A4XX_CP_PFP_UCODE_ADDR          0x223
#define A4XX_CP_PFP_UCODE_DATA          0x224
#define A4XX_CP_ME_RAM_WADDR            0x225
#define A4XX_CP_ME_RAM_DATA             0x227
#define A4XX_CP_DEBUG                   0x22c
#define A4XX_CP_ME_CNTL                 0x22d

/* PM4 Packet Macros */
#define CP_TYPE3_PKT                    (3U << 30)
#define cp_type3_packet(op, cnt)        (CP_TYPE3_PKT | (((cnt) - 1) << 16) | (((op) & 0xff) << 8))

#define CP_ME_INIT                      0x48
#define CP_NOP                          0x10
#define CP_REG_RMW                      0x21
#define CP_INTERRUPT                    0x40
#define CP_WAIT_FOR_IDLE                0x26
#define CP_MEM_WRITE                    0x3d
#define CP_INDIRECT_BUFFER_PFD          0x37
#define CP_INDIRECT_BUFFER_PFE          0x3f
#define CP_BOOTSTRAP_UCODE              0x6f
#define CP_EVENT_WRITE                  0x46
#define CP_WAIT_REG_MEM                 0x3c
#define CACHE_FLUSH_TS                  4

/* RPM: GPU core clock is owned by the RPM on MSM8994 (clk2 id 1, KHz).
 * The RPM scales VDD_GFX itself from this vote (Linux never raises the corner). */
#define OP2_RPM_GFX3D_CLK_ID            1U

/* Fence modes, chosen by the boot probe */
enum { kOP2FenceEvent = 0, kOP2FenceMemWrite = 1, kOP2FenceRptr = 2 };

/* Ringbuffer Configuration */
#define OP2_GPU_RB_SIZE                 (32 * 1024)     /* 32 KB */
#define OP2_GPU_RB_DWORDS               (OP2_GPU_RB_SIZE / 4)
#define OP2_GPU_MAX_BUFFERS             256

/* DRM MSM Constants (Linux msm_drm.h ABI compatibility) */
enum {
	kDrmGemClose            = 0x09,
	kMsmGetParam            = 0x40,
	kMsmSetParam            = 0x41,
	kMsmGemNew              = 0x42,
	kMsmGemInfo             = 0x43,
	kMsmGemCpuPrep          = 0x44,
	kMsmGemCpuFini          = 0x45,
	kMsmGemSubmit           = 0x46,
	kMsmWaitFence           = 0x47,
	kMsmGemMadvise          = 0x48,
	kMsmSubmitQueueNew      = 0x4a,
	kMsmSubmitQueueClose    = 0x4b,
	kMsmSubmitQueueQuery    = 0x4c,
	kOP2Log                 = 0x7c,
	kOP2UserMemory          = 0x7d,
	kOP2Surface             = 0x7e,
	kOP2Show                = 0x7f,
};

#define MSM_PARAM_GPU_ID        0x01
#define MSM_PARAM_GMEM_SIZE     0x02
#define MSM_PARAM_CHIP_ID       0x03
#define MSM_PARAM_MAX_FREQ      0x04
#define MSM_PARAM_TIMESTAMP     0x05
#define MSM_PARAM_GMEM_BASE     0x06

#define MSM_INFO_GET_OFFSET     0x00
#define MSM_INFO_GET_IOVA       0x01

#define MSM_SUBMIT_CMD_BUF      0x01

struct msm_param {
	uint32_t pipe, param;
	uint64_t value;
	uint32_t len, pad;
};

struct msm_gem_new {
	uint64_t size;
	uint32_t flags, handle;
};

struct msm_gem_info {
	uint32_t handle, info;
	uint64_t value;
	uint32_t len, pad;
};

struct msm_timespec {
	int64_t seconds, nanoseconds;
};

struct msm_gem_cpu_prep {
	uint32_t handle, op;
	struct msm_timespec timeout;
};

struct msm_gem_submit_cmd {
	uint32_t type, submit_idx, submit_offset, size, pad, nr_relocs;
	uint64_t relocs;
};

struct msm_gem_submit_bo {
	uint32_t flags, handle;
	uint64_t presumed;
};

struct msm_gem_submit {
	uint32_t flags, fence, nr_bos, nr_cmds;
	uint64_t bos, cmds;
	int32_t fence_fd;
	uint32_t queueid;
	uint64_t in_syncobjs, out_syncobjs;
	uint32_t nr_in_syncobjs, nr_out_syncobjs, syncobj_stride, pad;
};

struct msm_wait_fence {
	uint32_t fence, flags;
	struct msm_timespec timeout;
	uint32_t queueid;
};

struct drm_gem_close {
	uint32_t handle, pad;
};

struct op2_surface_req {
	uint32_t surface_id;
	uint32_t handle;
	uint64_t phys_addr;
	uint64_t size;
	uint32_t width;
	uint32_t height;
	uint32_t stride;
	uint32_t format;
};

extern "C" void op2_fb_kick_panel_external(void);

class OP2GPUUserClient;

class OP2GPU : public IOService {
	OSDeclareDefaultStructors(OP2GPU);
	friend class OP2GPUUserClient;

private:
	vm_offset_t fMMSSBase;
	vm_offset_t fGPUBase;

	uint32_t   *fRingBufferVirt;
	uint64_t    fRingBufferPhys;
	uint32_t    fWPTR;

	uint32_t   *fStatusPageVirt;
	uint64_t    fStatusPagePhys;
	uint32_t    fFenceSeq;
	uint32_t    fFenceMode;
	uint32_t    fHangs;
	uint32_t    fSubmits;
	uint32_t    fCoreMHz;

	bool        fEnabled;
	bool        fHardwareInitialized;
	IOLock     *fLock;

	void        writeMMSS(uint32_t byte_offset, uint32_t val);
	uint32_t    readMMSS(uint32_t byte_offset);

	void        writeGPU(uint32_t word_offset, uint32_t val);
	uint32_t    readGPU(uint32_t word_offset);

	bool        uncollapseGDSC(uint32_t byte_offset, const char *name);
	bool        enableBranchClock(uint32_t byte_offset, const char *name);
	bool        powerOnClocks(void);

	bool        initAdrenoRegisters(void);
	bool        bootstrapMicrocode(void);
	bool        allocRingBuffer(void);
	void        programRingBuffer(void);
	bool        meInit(void);
	bool        hwStart(void);
	bool        submitPacket(const uint32_t *words, uint32_t count);
	bool        waitIdle(uint32_t timeout_us);
	uint32_t    ringFree(void);
	void        dumpState(const char *why);
	uint32_t    measureCoreMHz(void);
	void        voteCoreClock(uint32_t khz);
	void        probeSMMU(void);
	bool        restoreSMMUSecureConfig(void);
	void        probeFences(void);
	void        selfTest(uint32_t count);
	void        selfTestG1(void);
	bool        runIB(uint32_t *ib, uint64_t ib_phys, uint32_t words, uint32_t timeout_ms, uint64_t *us_out);
	uint32_t    emitFence(uint32_t *cmds, uint32_t fence);
	uint32_t    readFenceMem(void);

public:
	virtual bool init(OSDictionary *dictionary = NULL) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;

	virtual IOReturn newUserClient(task_t owningTask, void *securityID,
	    UInt32 type, OSDictionary *properties,
	    IOUserClient **handler) APPLE_KEXT_OVERRIDE;
	virtual IOReturn newUserClient(task_t owningTask, void *securityID,
	    UInt32 type, IOUserClient **handler) APPLE_KEXT_OVERRIDE;

	bool submitIndirectBuffer(uint64_t phys_addr, uint32_t words, uint32_t *out_fence);
	bool recover(const char *why);
	bool waitForFence(uint32_t fence, uint32_t timeout_ms);
	bool isFenceDone(uint32_t fence);
	bool isHardwareReady(void) const { return fHardwareInitialized; }

	static OP2GPU *getInstance(void);
};

struct op2_gpu_client_buffer {
	IOMemoryDescriptor       *desc;
	void                     *kernel_addr;
	uint64_t                  phys_addr;
	uint64_t                  size;
	uint32_t                  flags;
	uint32_t                  fence;
	bool                      in_use;
	bool                      owned;       /* allocated by gemNew (not an IOSurface's memory) */
	uint64_t                  alloc_size;  /* size plus the guard band after it */
};

class OP2GPUUserClient : public IOUserClient {
	OSDeclareDefaultStructors(OP2GPUUserClient);

private:
	task_t                   fTask;
	OP2GPU                  *fOwner;
	IOLock                  *fLock;
	op2_gpu_client_buffer    fBuffers[OP2_GPU_MAX_BUFFERS];
	uint32_t                 fBufferCount;

	struct op2_gpu_client_buffer *getBuffer(uint32_t handle);
	IOReturn gemNew(struct msm_gem_new *req);
	IOReturn gemInfo(struct msm_gem_info *req);
	IOReturn gemCpuPrep(struct msm_gem_cpu_prep *req);
	IOReturn gemCpuFini(uint32_t handle);
	IOReturn gemSubmit(struct msm_gem_submit *req);
	IOReturn gemClose(uint32_t handle);
	void releaseBuffer(struct op2_gpu_client_buffer *b, uint32_t handle);
	IOReturn waitFence(struct msm_wait_fence *req);

public:
	virtual bool initWithTask(task_t owningTask, void *securityID,
	    UInt32 type, OSDictionary *properties) APPLE_KEXT_OVERRIDE;
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual void free(void) APPLE_KEXT_OVERRIDE;

	virtual IOReturn clientMemoryForType(UInt32 type,
	    IOOptionBits *options,
	    IOMemoryDescriptor **memory) APPLE_KEXT_OVERRIDE;

	virtual IOReturn externalMethod(uint32_t selector,
	    IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch,
	    OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
};

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
#endif /* _OP2_GPU_H */
