/*
 * Kernel Adreno 430 GPU Driver for OnePlus 2 (MSM8994).
 *
 * Implements Phase 2 of GPU bring-up:
 * 1. MMSSCC clock branches & GDSC power domain uncollapse
 * 2. Adreno 430 MMIO mapping (0xfdb00000, 256 KB)
 * 3. 32 KB wired command ring buffer allocation
 * 4. Microcode firmware bootstrapping (a420_pm4.fw, a420_pfp.fw)
 * 5. Hardware command processing & PM4 Type 3 packet execution
 */

#include <IOKit/IOSubMemoryDescriptor.h>
#include "OP2GPU.h"
#include "OP2Surface.h"
#include "op2_a430_fw.h"
#include "op2_rpm.h"
#include <pexpert/pexpert.h>
#include <libkern/OSByteOrder.h>
#include <sys/systm.h>
#include <mach/mach_time.h>
#include <kern/clock.h>

#ifndef IOMallocData
#define IOMallocData(sz) IOMalloc(sz)
#endif
#ifndef IOFreeData
#define IOFreeData(p, sz) IOFree((p), (sz))
#endif

#if defined(ARM64_BOARD_CONFIG_MSM8994)

OSDefineMetaClassAndStructors(OP2GPU, IOService);
OSDefineMetaClassAndStructors(OP2GPUUserClient, IOUserClient);

static OP2GPU *gOP2GPUInstance = NULL;

OP2GPU *
OP2GPU::getInstance(void)
{
	return gOP2GPUInstance;
}

extern "C" {
vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
}

void
OP2GPU::writeMMSS(uint32_t byte_offset, uint32_t val)
{
	if (!fMMSSBase) return;
	volatile uint32_t *reg = (volatile uint32_t *)(fMMSSBase + byte_offset);
	*reg = val;
	OSMemoryBarrier();
}

uint32_t
OP2GPU::readMMSS(uint32_t byte_offset)
{
	if (!fMMSSBase) return 0;
	OSMemoryBarrier();
	volatile uint32_t *reg = (volatile uint32_t *)(fMMSSBase + byte_offset);
	return *reg;
}

void
OP2GPU::writeGPU(uint32_t word_offset, uint32_t val)
{
	if (!fGPUBase) return;
	volatile uint32_t *reg = (volatile uint32_t *)(fGPUBase + (word_offset << 2));
	*reg = val;
	OSMemoryBarrier();
}

uint32_t
OP2GPU::readGPU(uint32_t word_offset)
{
	if (!fGPUBase) return 0;
	OSMemoryBarrier();
	volatile uint32_t *reg = (volatile uint32_t *)(fGPUBase + (word_offset << 2));
	return *reg;
}

bool
OP2GPU::uncollapseGDSC(uint32_t byte_offset, const char *name)
{
	IOLog("OP2 GPU: reading GDSC %s (offset 0x%04x)...\n", name, byte_offset);
	uint32_t val = readMMSS(byte_offset);
	IOLog("OP2 GPU: GDSC %s initial value = 0x%08x\n", name, val);

	if ((val & GDSC_PWR_ON_BIT) && !(val & GDSC_SW_COLLAPSE_BIT)) {
		IOLog("OP2 GPU: GDSC %s already uncollapsed (0x%08x)\n", name, val);
		return true;
	}

	/* Clear SW_COLLAPSE bit to power on domain */
	val &= ~GDSC_SW_COLLAPSE_BIT;
	writeMMSS(byte_offset, val);

	/* Poll for PWR_ON bit to be set (up to 100 tries of 10us = 1ms) */
	for (int i = 0; i < 100; i++) {
		val = readMMSS(byte_offset);
		if (val & GDSC_PWR_ON_BIT) {
			IOLog("OP2 GPU: GDSC %s successfully uncollapsed (0x%08x, took %d us)\n",
			      name, val, i * 10);
			return true;
		}
		IODelay(10);
	}

	IOLog("OP2 GPU: WARNING: GDSC %s timeout waiting for PWR_ON (0x%08x)\n", name, val);
	return false;
}

bool
OP2GPU::enableBranchClock(uint32_t byte_offset, const char *name)
{
	IOLog("OP2 GPU: reading clock %s (offset 0x%04x)...\n", name, byte_offset);
	uint32_t val = readMMSS(byte_offset);
	IOLog("OP2 GPU: clock %s initial value = 0x%08x\n", name, val);

	if ((val & CBCR_CLK_ENABLE_BIT) && !(val & CBCR_CLK_OFF_BIT)) {
		IOLog("OP2 GPU: Clock %s already active (0x%08x)\n", name, val);
		return true;
	}

	/* Write CLK_ENABLE bit */
	writeMMSS(byte_offset, val | CBCR_CLK_ENABLE_BIT);

	/* Poll for CLK_OFF bit to become 0 (up to 100 tries of 10us = 1ms) */
	for (int i = 0; i < 100; i++) {
		val = readMMSS(byte_offset);
		if (!(val & CBCR_CLK_OFF_BIT)) {
			IOLog("OP2 GPU: Clock %s enabled (0x%08x, took %d us)\n",
			      name, val, i * 10);
			return true;
		}
		IODelay(10);
	}

	IOLog("OP2 GPU: WARNING: Clock %s timeout waiting for CLK_OFF==0 (0x%08x)\n", name, val);
	return false;
}

bool
OP2GPU::powerOnClocks(void)
{
	bool ok = true;

	IOLog("OP2 GPU: Requesting VDD_GFX (PMI8994 S2) power rail uncollapse via RPM...\n");
	if (op2_rpm_power_on_gfx()) {
		IOLog("OP2 GPU: VDD_GFX power rail enabled successfully via RPM!\n");
	} else {
		IOLog("OP2 GPU: WARNING: op2_rpm_power_on_gfx failed or unacknowledged!\n");
	}

	IOLog("OP2 GPU: probing MMSS GDSC power domains...\n");
	if (!uncollapseGDSC(MMSS_OXILI_CX_GDSC, "oxili_cx")) {
		ok = false;
	}
	if (!uncollapseGDSC(MMSS_OXILI_GX_GDSC, "oxili_gx")) {
		ok = false;
	}

	IOLog("OP2 GPU: probing MMSS branch clocks...\n");
	if (!enableBranchClock(MMSS_OXILICX_AHB_CBCR, "oxilicx_ahb")) {
		ok = false;
	}
	if (!enableBranchClock(MMSS_OXILI_GFX3D_CBCR, "oxili_gfx3d")) {
		ok = false;
	}
	if (!enableBranchClock(MMSS_OXILI_RBBMTIMER_CBCR, "oxili_rbbmtimer")) {
		ok = false;
	}

	return ok;
}

static inline void
flushDcache(vm_offset_t va, vm_size_t size)
{
	vm_offset_t addr = va & ~63ULL;
	vm_offset_t end = (va + size + 63ULL) & ~63ULL;
	__asm__ volatile("dsb sy" ::: "memory");
	while (addr < end) {
		__asm__ volatile("dc civac, %0" : : "r"(addr) : "memory");
		addr += 64;
	}
	__asm__ volatile("dsb sy" ::: "memory");
}

/*
 * ---- G0: core bring-up, ring, fences, recovery, self-test (2026-11-11) ----
 * Sequence follows KGSL a4xx_start()/adreno_ringbuffer.c for A430
 * (reference/kernel-oneplus-msm8994/drivers/gpu/msm).
 */

static uint64_t
op2_gpu_ticks_to_us(uint64_t ticks)
{
	uint64_t ns = 0;
	absolutetime_to_nanoseconds(ticks, &ns);
	return ns / 1000ULL;
}


/*
 * SMC into TrustZone, SMCCC 32-bit SIP convention as OP2CPU.cpp uses it
 * (scm_call2: x0 fn, x1 arginfo, x2.. args; x0 status, x1.. results).
 * Reissued while TZ answers SCM_INTERRUPTED (1), bounded.
 */
static int32_t
op2_gpu_scm(uint32_t fn_id, uint32_t arginfo, uint32_t a0, uint32_t a1, uint32_t a2,
    uint32_t *ret0, uint32_t *ret1)
{
	register uint32_t r0 __asm__("w0");
	register uint32_t r1 __asm__("w1");
	register uint32_t r2 __asm__("w2");
	register uint32_t r3 __asm__("w3");
	register uint32_t r4 __asm__("w4");
	register uint32_t r5 __asm__("w5");
	register uint32_t r6 __asm__("w6");
	uint32_t tries = 0;
	do {
		r0 = fn_id;
		r1 = arginfo;
		r2 = a0;
		r3 = a1;
		r4 = a2;
		r5 = 0;
		r6 = 0;
		__asm__ volatile(
			"smc #0\n"
			: "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3)
			: "r"(r4), "r"(r5), "r"(r6)
			: "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "cc", "memory");
	} while ((int32_t)r0 == 1 && ++tries < 1000);
	if (ret0) *ret0 = r1;
	if (ret1) *ret1 = r2;
	return (int32_t)r0;
}

static int32_t
op2_gpu_scm2(uint32_t fn_id, uint32_t a0, uint32_t a1, uint32_t *ret0)
{
	return op2_gpu_scm(fn_id, 2, a0, a1, 0, ret0, NULL);
}

/* SCM_SIP_FNID(svc, cmd) = 0x02000000 | svc << 8 | cmd (32-bit SIP) */
#define OP2_SCM_FN(svc, cmd)       (0x02000000U | ((svc) << 8) | (cmd))
#define OP2_SCM_SVC_INFO           0x6U
#define OP2_SCM_SVC_MP             0xCU
#define OP2_SCM_MAKE_VERSION(a, b, c) ((((a) & 0x3ff) << 22) | (((b) & 0x3ff) << 12) | ((c) & 0xfff))

/*
 * One-time secure page-table pool for TZ-owned SMMUs (msm_iommu_sec_ptbl_init).
 * The pool belongs to TZ afterwards: never touched or freed by us.
 */
static bool
op2_gpu_sec_ptbl_init(void)
{
	static int done = 0;   /* 0 not tried, 1 ok, -1 failed */
	if (done) return done > 0;
	done = -1;

	uint32_t avail = 0, ver = 0;
	int32_t rc = op2_gpu_scm(OP2_SCM_FN(OP2_SCM_SVC_INFO, 1), 1,
	    OP2_SCM_FN(OP2_SCM_SVC_INFO, 3), 0, 0, &avail, NULL);   /* is GET_FEAT_VERSION available */
	if (rc == 0 && avail) {
		rc = op2_gpu_scm(OP2_SCM_FN(OP2_SCM_SVC_INFO, 3), 1, OP2_SCM_SVC_MP, 0, 0, &ver, NULL);
	}
	IOLog("OP2 GPU: TZ MP feature version 0x%x (%u.%u.%u) avail=%u rc=%d\n",
	      ver, ver >> 22, (ver >> 12) & 0x3ff, ver & 0xfff, avail, rc);

	if (ver >= OP2_SCM_MAKE_VERSION(1, 1, 1)) {
		uint32_t r = 0;
		rc = op2_gpu_scm(OP2_SCM_FN(OP2_SCM_SVC_MP, 5), 2, 300U << 20, 0, 0, &r, NULL);
		IOLog("OP2 GPU: SMC SET_CP_POOL_SIZE(300 MB): rc=%d ret=%u\n", rc, r);
		if (rc != 0) return false;
	}

	uint32_t size = 0, status = 0;
	rc = op2_gpu_scm(OP2_SCM_FN(OP2_SCM_SVC_MP, 3), 1, 0, 0, 0, &size, &status);
	IOLog("OP2 GPU: SMC PTBL_SIZE: rc=%d size=%u status=%u\n", rc, size, status);
	if (rc != 0 || status != 0 || size == 0 || size > (16U << 20)) return false;

	uint32_t alloc = (size + 0xffffU) & ~0xffffU;
	IOPhysicalAddress phys = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	void *pool = IOMallocContiguous(alloc, 0x10000, &phys);
#pragma clang diagnostic pop
	if (!pool) {
		IOLog("OP2 GPU: cannot allocate %u bytes for TZ page tables\n", alloc);
		return false;
	}
	bzero(pool, alloc);
	flushDcache((vm_offset_t)pool, alloc);

	uint32_t pret = 0xdead;
	/* SCM_ARGS(3, SCM_RW, SCM_VAL, SCM_VAL) = 3 | 2 << 4 */
	rc = op2_gpu_scm(OP2_SCM_FN(OP2_SCM_SVC_MP, 4), 0x23, (uint32_t)phys, size, 0, &pret, NULL);
	IOLog("OP2 GPU: SMC PTBL_INIT(phys 0x%llx, %u): rc=%d ret=%u\n", (uint64_t)phys, size, rc, pret);
	if (rc != 0 || pret != 0) {
		return false;   /* leak the pool: TZ may already own part of it */
	}
	done = 1;
	return true;
}

/* SCM_SIP_FNID(SCM_SVC_MP 0xC, RESTORE_SEC_CFG 2), 32-bit SIP owner */
#define OP2_SCM_RESTORE_SEC_CFG   0x02000C02U
#define OP2_GPU_SMMU_SEC_ID       18U          /* msm8994-iommu.dtsi kgsl_iommu */

bool
OP2GPU::restoreSMMUSecureConfig(void)
{
	if (!op2_gpu_sec_ptbl_init()) {
		IOLog("OP2 GPU: TZ page-table pool not set up; trying restore_sec_cfg anyway\n");
	}
	/* contexts KGSL attaches: 0 = gfx3d_user (sid 0), 1 = gfx3d_priv (sid 1) */
	bool ok = true;
	for (uint32_t ctx = 0; ctx < 2; ctx++) {
		uint32_t scm_ret = 0xdead;
		int32_t rc = op2_gpu_scm2(OP2_SCM_RESTORE_SEC_CFG, OP2_GPU_SMMU_SEC_ID, ctx, &scm_ret);
		IOLog("OP2 GPU: SMC restore_sec_cfg(%u, ctx %u): rc=%d scm_ret=%u\n",
		      OP2_GPU_SMMU_SEC_ID, ctx, rc, scm_ret);
		if (rc != 0 || scm_ret != 0) ok = false;
	}
	return ok;
}

static uint32_t gOP2GPUInitSteps = 0x11;   /* gpu_init= bitmask (see initAdrenoRegisters): VBIF + SP/TP power (shaders need it; G3) */

#define OP2_GPU_CP(n) do { if (fHangs == 0) IOLog("OP2 GPU: init checkpoint %s\n", n); } while (0)

/*
 * gpu_init bits: 1 VBIF, 2 hang-detect, 4 UCHE, 8 SP regfile sleep, 16 SP/TP power
 * (checkpoints are logged on the first start only, so a bus stall names its block)
 */
bool
OP2GPU::initAdrenoRegisters(void)
{
	writeGPU(A4XX_RBBM_SW_RESET_CMD, 1);
	(void)readGPU(A4XX_RBBM_SW_RESET_CMD);
	IODelay(100);
	writeGPU(A4XX_RBBM_SW_RESET_CMD, 0);
	IODelay(100);
	/* No GPU IRQ handler yet: keep every source masked, clear latched ones */
	writeGPU(A4XX_RBBM_INT_0_MASK, 0);
	writeGPU(A4XX_RBBM_INT_CLEAR_CMD, 0xffffffff);
	OP2_GPU_CP("reset");

	writeGPU(A4XX_RBBM_GPU_BUSY_MASKED, 0xffffffff);
	writeGPU(A4XX_RBBM_SP_HYST_CNT, 0x10);
	writeGPU(A4XX_RBBM_WAIT_IDLE_CLOCKS_CTL, 0x10);
	writeGPU(A4XX_RBBM_WAIT_IDLE_CLOCKS_CTL2, 0x30);
	writeGPU(A4XX_RBBM_AHB_CTL0, 0x00000001);
	writeGPU(A4XX_RBBM_AHB_CTL1, 0xa6ffffff);
	writeGPU(A4XX_RBBM_RBBM_CTL, 0x00000030);
	writeGPU(A4XX_RBBM_PERFCTR_CTL, 0x01);
	writeGPU(A4XX_RBBM_PERFCTR_RBBM_SEL_0, 0);   /* RBBM_ALWAYS_COUNT = core cycles */
	writeGPU(A4XX_RB_GMEM_BASE_ADDR, 0);         /* GMEM = OCMEM, not allocated: sysmem only */
	writeGPU(A4XX_CP_DEBUG, (1U << 25) | (1U << 14));   /* A430 chicken bit during ucode load */
	OP2_GPU_CP("rbbm");

	if (gOP2GPUInitSteps & 2) {
		writeGPU(A4XX_RBBM_INTERFACE_HANG_INT_CTL, (1U << 30) | 0xffff);
		OP2_GPU_CP("hang-detect");
	}
	if (fHangs == 0) {
		OP2_GPU_CP("vbif read");
		IOLog("OP2 GPU: VBIF VERSION=0x%08x CLKON=0x%08x GATE_OFF_WRREQ=0x%x RD_LIM=0x%08x/0x%08x WR_LIM=0x%08x/0x%08x QOS=0x%x XIN_HALT_CTRL0=0x%x CTRL1=0x%x\n",
		      readGPU(0x3000), readGPU(0x3001), readGPU(A4XX_VBIF_GATE_OFF_WRREQ_EN),
		      readGPU(A4XX_VBIF_IN_RD_LIM_CONF0), readGPU(A4XX_VBIF_IN_RD_LIM_CONF1),
		      readGPU(A4XX_VBIF_IN_WR_LIM_CONF0), readGPU(A4XX_VBIF_IN_WR_LIM_CONF1),
		      readGPU(A4XX_VBIF_ROUND_ROBIN_QOS_ARB),
		      readGPU(A4XX_VBIF_XIN_HALT_CTRL0), readGPU(A4XX_VBIF_XIN_HALT_CTRL1));
	}
	if (readGPU(A4XX_VBIF_XIN_HALT_CTRL0) & 0x1f) {
		writeGPU(A4XX_VBIF_XIN_HALT_CTRL0, 0);   /* un-halt the bus ports */
		IOLog("OP2 GPU: VBIF ports were halted; cleared -> CTRL1=0x%x\n", readGPU(A4XX_VBIF_XIN_HALT_CTRL1));
	}
	if (gOP2GPUInitSteps & 1) {   /* a430_vbif[] */
		writeGPU(A4XX_VBIF_GATE_OFF_WRREQ_EN, 0x00000001);
		writeGPU(A4XX_VBIF_IN_RD_LIM_CONF0, 0x18181818);
		writeGPU(A4XX_VBIF_IN_RD_LIM_CONF1, 0x00000018);
		writeGPU(A4XX_VBIF_IN_WR_LIM_CONF0, 0x18181818);
		writeGPU(A4XX_VBIF_IN_WR_LIM_CONF1, 0x00000018);
		writeGPU(A4XX_VBIF_ROUND_ROBIN_QOS_ARB, 0x00000003);
		OP2_GPU_CP("vbif");
	}
	if (gOP2GPUInitSteps & 4) {
		writeGPU(A4XX_UCHE_CACHE_WAYS_VFD, 0x07);
		writeGPU(A4XX_UCHE_TRAP_BASE_LO, 0xffff0000);
		writeGPU(A4XX_UCHE_TRAP_BASE_HI, 0xffff0000);
		OP2_GPU_CP("uche");
	}
	if (gOP2GPUInitSteps & 8) {
		writeGPU(A4XX_RBBM_SP_REGFILE_SLEEP_CNTL_0, 0x00000441);
		writeGPU(A4XX_RBBM_SP_REGFILE_SLEEP_CNTL_1, 0x00000441);
		OP2_GPU_CP("sp-regfile");
	}
	uint32_t pwr = readGPU(A4XX_RBBM_POWER_STATUS);
	bool sptp = (pwr & A4XX_SP_TP_PWR_ON) != 0;
	if ((gOP2GPUInitSteps & 16) && !sptp) {
		/* A430 SP/TP power inside the core (a4xx_regulator_enable; off after soft reset) */
		writeGPU(A4XX_RBBM_POWER_CNTL_IP, 0x778000);
		for (int i = 0; i < 2000; i++) {   /* 10 ms */
			pwr = readGPU(A4XX_RBBM_POWER_STATUS);
			if (pwr & A4XX_SP_TP_PWR_ON) { sptp = true; break; }
			IODelay(5);
		}
		OP2_GPU_CP("sp-tp");
	}
	if (fHangs == 0) {
		IOLog("OP2 GPU: core reset; RBBM_STATUS=0x%08x POWER_STATUS=0x%08x SP/TP=%s\n",
		      readGPU(A4XX_RBBM_STATUS), pwr, sptp ? "on" : "off");
	}
	return true;
}

bool
OP2GPU::allocRingBuffer(void)
{
	IOPhysicalAddress phys = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	fRingBufferVirt = (uint32_t *)IOMallocContiguous(OP2_GPU_RB_SIZE, 4096, &phys);
#pragma clang diagnostic pop
	if (!fRingBufferVirt) {
		IOLog("OP2 GPU: ERROR: failed to allocate ringbuffer\n");
		return false;
	}
	fRingBufferPhys = (uint64_t)phys;

	IOPhysicalAddress status_phys = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	fStatusPageVirt = (uint32_t *)IOMallocContiguous(4096, 4096, &status_phys);
#pragma clang diagnostic pop
	if (!fStatusPageVirt) {
		IOLog("OP2 GPU: ERROR: failed to allocate status page\n");
		return false;
	}
	fStatusPagePhys = (uint64_t)status_phys;
	bzero(fStatusPageVirt, 4096);
	flushDcache((vm_offset_t)fStatusPageVirt, 4096);
	IOLog("OP2 GPU: ring phys=0x%llx (%u dwords), status phys=0x%llx\n",
	      fRingBufferPhys, OP2_GPU_RB_DWORDS, fStatusPagePhys);
	return true;
}

void
OP2GPU::programRingBuffer(void)
{
	/* Fill with 0xAA like KGSL so a stray fetch is recognisable */
	memset(fRingBufferVirt, 0xaa, OP2_GPU_RB_SIZE);
	flushDcache((vm_offset_t)fRingBufferVirt, OP2_GPU_RB_SIZE);
	fWPTR = 0;
	/* size = log2(quadwords); bit 27 = no RPTR shadow (read the register) */
	writeGPU(A4XX_CP_RB_CNTL, (12 & 0x3f) | (1U << 27));
	writeGPU(A4XX_CP_RB_BASE, (uint32_t)fRingBufferPhys);
	writeGPU(A4XX_CP_RB_WPTR, 0);
	writeGPU(A4XX_CP_RB_RPTR, 0);
}

uint32_t
OP2GPU::ringFree(void)
{
	uint32_t rptr = readGPU(A4XX_CP_RB_RPTR) % OP2_GPU_RB_DWORDS;
	return (rptr + OP2_GPU_RB_DWORDS - fWPTR - 1) % OP2_GPU_RB_DWORDS;
}

bool
OP2GPU::waitIdle(uint32_t timeout_us)
{
	for (uint32_t t = 0; t < timeout_us; t += 10) {
		if (readGPU(A4XX_CP_RB_RPTR) == fWPTR) {
			return true;
		}
		IODelay(10);
	}
	return false;
}

void
OP2GPU::dumpState(const char *why)
{
	IOLog("OP2 GPU: [%s] RPTR=%u WPTR=%u RBBM_STATUS=0x%08x INT0=0x%08x AHB_ERR=0x%08x "
	      "ME_STAT=0x%08x HW_FAULT=0x%08x PROT=0x%08x fence(mem)=%u seq=%u\n",
	      why, readGPU(A4XX_CP_RB_RPTR), fWPTR, readGPU(A4XX_RBBM_STATUS),
	      readGPU(A4XX_RBBM_INT_0_STATUS), readGPU(A4XX_RBBM_AHB_ERROR_STATUS),
	      readGPU(A4XX_CP_ME_STATUS), readGPU(A4XX_CP_HW_FAULT),
	      readGPU(A4XX_CP_PROTECT_STATUS), readFenceMem(), fFenceSeq);
	checkSMMUFault(why);
}

/*
 * Write a packet into the circular ring. Packets may straddle the end; the CP
 * wraps per dword. Caller holds fLock (or is single-threaded in start()).
 */
bool
OP2GPU::submitPacket(const uint32_t *words, uint32_t count)
{
	if (!fRingBufferVirt || count == 0 || count >= OP2_GPU_RB_DWORDS / 2) {
		return false;
	}
	uint32_t waited = 0;
	while (ringFree() < count) {
		if (waited >= 200000) {
			dumpState("ring full");
			return false;
		}
		IODelay(20);
		waited += 20;
	}

	uint32_t start = fWPTR;
	for (uint32_t i = 0; i < count; i++) {
		fRingBufferVirt[fWPTR] = words[i];
		fWPTR = (fWPTR + 1) % OP2_GPU_RB_DWORDS;
	}
	if (start + count <= OP2_GPU_RB_DWORDS) {
		flushDcache((vm_offset_t)&fRingBufferVirt[start], count * 4);
	} else {
		flushDcache((vm_offset_t)&fRingBufferVirt[start], (OP2_GPU_RB_DWORDS - start) * 4);
		flushDcache((vm_offset_t)&fRingBufferVirt[0], fWPTR * 4);
	}
	OSMemoryBarrier();
	writeGPU(A4XX_CP_RB_WPTR, fWPTR);
	return true;
}

bool
OP2GPU::bootstrapMicrocode(void)
{
	/* ME_HALT (28) + PFP_HALT (27): what KGSL sets to stop the CP. Writing 1 halted nothing. */
	uint32_t cntl0 = readGPU(A4XX_CP_ME_CNTL);
	writeGPU(A4XX_CP_ME_CNTL, (1U << 27) | (1U << 28));
	IODelay(10);

	writeGPU(A4XX_CP_ME_RAM_WADDR, 0);
	for (uint32_t i = 1; i < OP2_A430_PM4_FW_WORDS; i++) {
		writeGPU(A4XX_CP_ME_RAM_DATA, op2_a430_pm4_fw[i]);
	}
	writeGPU(A4XX_CP_PFP_UCODE_ADDR, 0);
	for (uint32_t i = 1; i < OP2_A430_PFP_FW_WORDS; i++) {
		writeGPU(A4XX_CP_PFP_UCODE_DATA, op2_a430_pfp_fw[i]);
	}

	if (fHangs == 0) {
		/* read both RAMs back while halted */
		uint32_t me_bad = 0, pfp_bad = 0, me_first = 0, pfp_first = 0;
		writeGPU(0x226 /* CP_ME_RAM_RADDR */, 0);
		for (uint32_t i = 1; i < OP2_A430_PM4_FW_WORDS; i++) {
			uint32_t v = readGPU(A4XX_CP_ME_RAM_DATA);
			if (i == 1) me_first = v;
			if (v != op2_a430_pm4_fw[i]) me_bad++;
		}
		writeGPU(A4XX_CP_PFP_UCODE_ADDR, 0);
		for (uint32_t i = 1; i < OP2_A430_PFP_FW_WORDS; i++) {
			uint32_t v = readGPU(A4XX_CP_PFP_UCODE_DATA);
			if (i == 1) pfp_first = v;
			if (v != op2_a430_pfp_fw[i]) pfp_bad++;
		}
		IOLog("OP2 GPU: ucode readback: ME %u/%u wrong (first 0x%08x want 0x%08x), PFP %u/%u wrong (first 0x%08x want 0x%08x); ME_CNTL was 0x%08x\n",
		      me_bad, OP2_A430_PM4_FW_WORDS - 1, me_first, op2_a430_pm4_fw[1],
		      pfp_bad, OP2_A430_PFP_FW_WORDS - 1, pfp_first, op2_a430_pfp_fw[1], cntl0);
	}

	writeGPU(A4XX_CP_ME_CNTL, 0);
	IODelay(100);
	return true;
}

bool
OP2GPU::meInit(void)
{
	static const uint32_t me_init_cmds[] = {
		cp_type3_packet(CP_ME_INIT, 17),
		0x000003f7, 0x00000000, 0x00000000, 0x00000000,
		0x00000080, 0x00000100, 0x00000180, 0x00006600,
		0x00000150, 0x0000014e, 0x00000154, 0x00000001,
		0x00000000, 0x00000000,
		0x20000000,              /* protected mode registers */
		0x00000000, 0x00000000
	};
	if (!submitPacket(me_init_cmds, sizeof(me_init_cmds) / 4)) {
		return false;
	}
	OP2_GPU_CP("me-init submitted");
	bool ok = waitIdle(50000);
	OP2_GPU_CP(ok ? "me-init idle" : "me-init TIMEOUT");
	/* clear the A430 bootstrap chicken bit after init (KGSL does the same) */
	writeGPU(A4XX_CP_DEBUG, (1U << 25));
	return ok;
}

/* Full (re)start of the core. Caller holds fLock or is in start(). */
bool
OP2GPU::hwStart(void)
{
	initAdrenoRegisters();
	if (fHangs == 0) fCoreMHz = measureCoreMHz();
	programRingBuffer();
	OP2_GPU_CP("ring");
	bootstrapMicrocode();
	OP2_GPU_CP("ucode");
	if (fHangs == 0) {
		IOLog("OP2 GPU: pre-ME_INIT RPTR=%u ME_CNTL=0x%08x RBBM_STATUS=0x%08x RB_BASE=0x%08x RB_CNTL=0x%08x\n",
		      readGPU(A4XX_CP_RB_RPTR), readGPU(A4XX_CP_ME_CNTL), readGPU(A4XX_RBBM_STATUS),
		      readGPU(A4XX_CP_RB_BASE), readGPU(A4XX_CP_RB_CNTL));
	}
	if (!meInit()) {
		dumpState("ME_INIT");
		return false;
	}
	return true;
}

uint32_t
OP2GPU::readFenceMem(void)
{
	if (!fStatusPageVirt) return 0;
	flushDcache((vm_offset_t)fStatusPageVirt, 64);
	return ((volatile uint32_t *)fStatusPageVirt)[0];
}

/* Append a fence write for `fence` to cmds; returns dwords written (<= 4). */
uint32_t
OP2GPU::emitFence(uint32_t *cmds, uint32_t fence)
{
	if (fFenceMode == kOP2FenceMemWrite) {
		cmds[0] = cp_type3_packet(CP_MEM_WRITE, 2);
		cmds[1] = (uint32_t)fStatusPagePhys;
		cmds[2] = fence;
		return 3;
	}
	/* CACHE_FLUSH_TS: flush caches, then write the timestamp */
	cmds[0] = cp_type3_packet(CP_EVENT_WRITE, 3);
	cmds[1] = CACHE_FLUSH_TS;
	cmds[2] = (uint32_t)fStatusPagePhys;
	cmds[3] = fence;
	return 4;
}

uint32_t
OP2GPU::measureCoreMHz(void)
{
	uint32_t gpu_mhz = 1;
	PE_parse_boot_argn("gpu_mhz", &gpu_mhz, sizeof(gpu_mhz));
	if (!gpu_mhz) return 0;
	OP2_GPU_CP("mhz: read PERFCTR_RBBM_0_LO");
	uint32_t c0 = readGPU(A4XX_RBBM_PERFCTR_RBBM_0_LO);
	OP2_GPU_CP("mhz: read ALWAYSON_LO");
	uint32_t a0 = readGPU(A4XX_RBBM_ALWAYSON_COUNTER_LO);
	uint64_t t0 = mach_absolute_time();
	OP2_GPU_CP("mhz: delay");
	IODelay(2000);
	uint32_t c1 = readGPU(A4XX_RBBM_PERFCTR_RBBM_0_LO);
	uint32_t a1 = readGPU(A4XX_RBBM_ALWAYSON_COUNTER_LO);
	uint64_t us = op2_gpu_ticks_to_us(mach_absolute_time() - t0);
	if (us == 0) us = 1;
	uint32_t mhz = (uint32_t)((uint32_t)(c1 - c0) / us);
	IOLog("OP2 GPU: core clock ~%u MHz (%u cycles / %llu us), always-on ~%llu kHz\n",
	      mhz, c1 - c0, us, ((uint64_t)(uint32_t)(a1 - a0) * 1000ULL) / us);
	return mhz;
}

void
OP2GPU::voteCoreClock(uint32_t khz)
{
	uint32_t key = OP2_RPM_KEY_KHZ;
	uint32_t val = khz;
	bool ok = op2_rpm_request(OP2_RPM_SET_ACTIVE, OP2_RPM_RSC_CLK2,
	    OP2_RPM_GFX3D_CLK_ID, &key, &val, 1);
	IOLog("OP2 GPU: RPM gfx3d_clk_src vote %u kHz: %s\n", khz, ok ? "ACK" : "FAIL");
}

/*
 * Context-bank registers only (0xfdb18000 + n * 0x1000). The global space
 * is TZ's: reading it reset the SoC (2026-11-11 r6). KGSL touches CBs after
 * restore_sec_cfg, so these should be NS-accessible once TZ configured them.
 */
/*
 * GPU SMMU context bank 0 (gfx3d_user, stream id 0), ARMv7 short-descriptor tables, identity (IOVA = physical),
 * 4 KB pages. Programmed after the same pattern as Linux msm_iommu-v1.c __program_context() for a TZ-owned
 * (secure) SMMU: only context-bank registers, never CBAR/SMR (global space, TZ's). Differences from Linux:
 *  - TEX remap off (SCTLR.TRE=0): TEX/C/B in the PTE are taken literally, so no PRRR/NMRR to copy from the CPU.
 *    Pages are Normal, inner+outer non-cacheable, shareable (TEX=001 C=0 B=0) = KGSL's uncached GPU mappings.
 *  - No interrupt (CFIE=0): FSR/FAR latch anyway and are polled while fences are awaited.
 *  - Faulting accesses are terminated as RAZ/WI (CFRE=0, CFCFG=0): a stray write is dropped, a stray read
 *    returns 0, the GPU keeps running. (CFRE=1 would hand the GPU a bus abort: risk of a VBIF lockup.)
 * KGSL on 8994 attaches only gfx3d_user (priv_bank_table is v0-only); CB1 (gfx3d_priv) stays as TZ left it.
 */
#define OP2_GPU_CB0_OFFSET   0x18000U     /* SMMU base (Adreno+0x10000) + 0x8000 */
#define OP2_CB_SCTLR         0x000U
#define OP2_CB_RESUME        0x008U
#define OP2_CB_TTBR0         0x020U
#define OP2_CB_TTBR1         0x028U
#define OP2_CB_TTBCR         0x030U
#define OP2_CB_CONTEXTIDR    0x034U
#define OP2_CB_PAR           0x050U
#define OP2_CB_FSR           0x058U
#define OP2_CB_FAR           0x060U
#define OP2_CB_FSYNR0        0x068U
#define OP2_CB_TLBIALL       0x618U
#define OP2_CB_TLBSYNC       0x7F0U
#define OP2_CB_TLBSTATUS     0x7F4U
#define OP2_CB_ATS1PR        0x800U
#define OP2_CB_ATSR          0x8F0U
/* SCTLR bits */
#define OP2_SCTLR_M          (1U << 0)
#define OP2_SCTLR_TRE        (1U << 1)
#define OP2_SCTLR_AFE        (1U << 2)
#define OP2_SCTLR_CFRE       (1U << 5)    /* 1: a context fault returns an abort; 0: RAZ/WI */
#define OP2_SCTLR_CFIE       (1U << 6)    /* fault interrupt */
#define OP2_SCTLR_CFCFG      (1U << 7)    /* 1: stall on fault; 0: terminate */
#define OP2_SCTLR_HUPCF      (1U << 8)
#define OP2_SCTLR_VALUE      (OP2_SCTLR_M)   /* TRE=0 AFE=0 CFRE=0 CFIE=0 CFCFG=0 */
/* FSR bits */
#define OP2_FSR_TF           (1U << 1)
#define OP2_FSR_AFF          (1U << 2)
#define OP2_FSR_PF           (1U << 3)
#define OP2_FSR_EF           (1U << 4)
#define OP2_FSR_MULTI        (1U << 31)
#define OP2_FSYNR0_WNR       (1U << 4)
/* first level: pointer to a second-level (coarse) table, base [31:10] */
#define OP2_FL_TABLE         0x1U
/* second level: small page, base [31:12] (msm_iommu_pagetable.c SL_* bits) */
#define OP2_SL_SMALL         0x2U
#define OP2_SL_AP0           (1U << 4)
#define OP2_SL_AP1           (1U << 5)
#define OP2_SL_TEX0          (1U << 6)
#define OP2_SL_SHARED        (1U << 10)
#define OP2_SL_ATTR          (OP2_SL_SMALL | OP2_SL_AP0 | OP2_SL_AP1 | OP2_SL_TEX0 | OP2_SL_SHARED)  /* RW, NC */
#define OP2_L2_PER_CHUNK     16U          /* 1 KB tables per 16 KB contiguous chunk */
#define OP2_L2_MAX_CHUNKS    256U         /* 4096 tables = all 4 GB */

/* SMMU bring-up lines go straight into the ramoops ring (op2_ramoops_ap_write): IOLog passes through the console
 * buffer, and a SoC reset (round g4d-1..3) loses its tail, so the log could not say where the reset happened. */
extern "C" void op2_ramoops_ap_write(const char *s);
#define OP2_SMMU_LOG(...) do { char _b[256]; snprintf(_b, sizeof(_b), __VA_ARGS__); op2_ramoops_ap_write(_b); } while (0)

static void    *gL2ChunkVirt[OP2_L2_MAX_CHUNKS];
static uint64_t gL2ChunkPhys[OP2_L2_MAX_CHUNKS];

void
OP2GPU::writeCB(uint32_t off, uint32_t val)
{
	volatile uint32_t *r = (volatile uint32_t *)(fGPUBase + OP2_GPU_CB0_OFFSET + off);
	*r = val;
	OSMemoryBarrier();
}

uint32_t
OP2GPU::readCB(uint32_t off)
{
	OSMemoryBarrier();
	return *(volatile uint32_t *)(fGPUBase + OP2_GPU_CB0_OFFSET + off);
}

void
OP2GPU::tlbInvalidateAll(void)
{
	writeCB(OP2_CB_TLBIALL, 0);
	writeCB(OP2_CB_TLBSYNC, 0);
	for (int i = 0; i < 1000; i++) {
		if (!(readCB(OP2_CB_TLBSTATUS) & 1)) return;
		IODelay(1);
	}
	IOLog("OP2 GPU SMMU: TLB sync did not finish (TLBSTATUS=0x%x)\n", readCB(OP2_CB_TLBSTATUS));
}

/* the second-level table for 1 MB section s; created (and hooked into the first level) on demand. fMapLock held. */
uint32_t *
OP2GPU::l2For(uint32_t s, bool create)
{
	if (fL2Virt[s] || !create) return fL2Virt[s];
	uint32_t chunk = fL2Used / OP2_L2_PER_CHUNK, slot = fL2Used % OP2_L2_PER_CHUNK;
	if (chunk >= OP2_L2_MAX_CHUNKS) return NULL;
	uint8_t *ref = (uint8_t *)IOMalloc(256);
	if (!ref) return NULL;
	if (slot == 0) {
		IOPhysicalAddress p = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		void *v = IOMallocContiguous(16384, 16384, &p);
#pragma clang diagnostic pop
		if (!v) {
			IOFree(ref, 256);
			return NULL;
		}
		bzero(v, 16384);
		flushDcache((vm_offset_t)v, 16384);
		gL2ChunkVirt[chunk] = v;
		gL2ChunkPhys[chunk] = (uint64_t)p;
	}
	bzero(ref, 256);
	uint32_t *l2 = (uint32_t *)gL2ChunkVirt[chunk] + slot * 256;   /* 1 KB per table */
	uint64_t l2phys = gL2ChunkPhys[chunk] + slot * 1024;
	fL2Used++;
	fL2Virt[s] = l2;
	fL2Ref[s] = ref;
	fPageTableVirt[s] = ((uint32_t)l2phys & 0xFFFFFC00U) | OP2_FL_TABLE;
	flushDcache((vm_offset_t)&fPageTableVirt[s], 4);
	return l2;
}

/* identity-map every 4 KB page [phys, phys+bytes) touches; refcounted (255 = sticky) so overlaps are safe */
void
OP2GPU::mapRange(uint64_t phys, uint64_t bytes)
{
	if (!fPageTableVirt || !bytes) return;
	uint64_t a = phys & ~0xFFFULL;
	uint64_t end = (phys + bytes + 0xFFFULL) & ~0xFFFULL;
	if (end > (1ULL << 32)) {
		IOLog("OP2 GPU SMMU: map 0x%llx+0x%llx beyond 4 GB: not mapped\n", phys, bytes);
		return;
	}
	IOLockLock(fMapLock);
	while (a < end) {
		uint32_t s = (uint32_t)(a >> 20);
		uint64_t send = ((uint64_t)s + 1) << 20;
		if (send > end) send = end;
		uint32_t *l2 = l2For(s, true);
		if (!l2) {
			IOLog("OP2 GPU SMMU: no second-level table for 0x%llx: rest of 0x%llx+0x%llx NOT mapped\n", a, phys, bytes);
			break;
		}
		uint8_t *ref = fL2Ref[s];
		uint32_t i0 = (uint32_t)((a >> 12) & 0xFF), i1 = (uint32_t)(((send - 1) >> 12) & 0xFF);
		for (uint32_t i = i0; i <= i1; i++) {
			if (ref[i] == 0) {
				l2[i] = ((s << 20) | (i << 12)) | OP2_SL_ATTR;
				fPagesMapped++;
			}
			if (ref[i] < 255) ref[i]++;
		}
		flushDcache((vm_offset_t)&l2[i0], (i1 - i0 + 1) * 4);
		a = send;
	}
	if (fXlate) tlbInvalidateAll();
	IOLockUnlock(fMapLock);
}

void
OP2GPU::unmapRange(uint64_t phys, uint64_t bytes)
{
	if (!fPageTableVirt || !bytes) return;
	uint64_t a = phys & ~0xFFFULL;
	uint64_t end = (phys + bytes + 0xFFFULL) & ~0xFFFULL;
	if (end > (1ULL << 32)) return;
	IOLockLock(fMapLock);
	while (a < end) {
		uint32_t s = (uint32_t)(a >> 20);
		uint64_t send = ((uint64_t)s + 1) << 20;
		if (send > end) send = end;
		uint32_t *l2 = l2For(s, false);
		if (l2) {
			uint8_t *ref = fL2Ref[s];
			uint32_t i0 = (uint32_t)((a >> 12) & 0xFF), i1 = (uint32_t)(((send - 1) >> 12) & 0xFF);
			for (uint32_t i = i0; i <= i1; i++) {
				if (ref[i] && ref[i] < 255 && --ref[i] == 0) {
					l2[i] = 0;   /* now faults */
					fPagesMapped--;
				}
			}
			flushDcache((vm_offset_t)&l2[i0], (i1 - i0 + 1) * 4);
		}
		a = send;
	}
	/* the TLB must forget the pages before the caller frees them */
	if (fXlate) tlbInvalidateAll();
	IOLockUnlock(fMapLock);
}

/* map/unmap a memory descriptor's first `bytes` segment by segment (an IOSurface need not be contiguous) */
void
OP2GPU::mapDescriptor(IOMemoryDescriptor *md, uint64_t bytes, bool map)
{
	uint32_t segs = 0;
	for (uint64_t off = 0; off < bytes;) {
		IOByteCount len = 0;
		uint64_t p = (uint64_t)md->getPhysicalSegment(off, &len, kIOMemoryMapperNone);
		if (!p || !len) {
			IOLog("OP2 GPU SMMU: descriptor has no segment at +0x%llx of 0x%llx\n", off, bytes);
			break;
		}
		if (len > bytes - off) len = (IOByteCount)(bytes - off);
		if (map) mapRange(p, len); else unmapRange(p, len);
		off += len;
		segs++;
	}
	if (map && segs > 1) {
		IOLog("OP2 GPU SMMU: descriptor of 0x%llx bytes mapped as %u physical segments\n", bytes, segs);
	}
}

bool
OP2GPU::isMapped(uint64_t phys)
{
	if (!fPageTableVirt || phys >= (1ULL << 32)) return false;
	uint32_t s = (uint32_t)(phys >> 20);
	return fL2Virt[s] && fL2Virt[s][(phys >> 12) & 0xFF] != 0;
}

/* read + clear one CB0 fault latch; records it. Returns the FSR (0 = none). Cheap when there is no fault. */
uint32_t
OP2GPU::pollSMMUFault(void)
{
	if (!fXlate) return 0;
	if (!readCB(OP2_CB_FSR)) return 0;
	IOLockLock(fMapLock);
	uint32_t fsr = readCB(OP2_CB_FSR);
	if (fsr) {
		uint32_t far = readCB(OP2_CB_FAR);   /* 32-bit IOVA space: FAR high word is 0 */
		uint32_t syn = readCB(OP2_CB_FSYNR0);
		if (fFaultCount < 4) {
			OP2_SMMU_LOG("OP2 GPU SMMU: latch #%u FSR 0x%08x FAR 0x%08x FSYNR0 0x%08x: clearing\n",
			             fFaultCount + 1, fsr, far, syn);
		}
		writeCB(OP2_CB_FSR, fsr);             /* write-1-to-clear: re-arms the latch */
		if (fFaultCount < 4) {
			OP2_SMMU_LOG("OP2 GPU SMMU: latch #%u cleared (FSR now 0x%08x)\n", fFaultCount + 1, readCB(OP2_CB_FSR));
		}
		uint32_t n = fFaultCount++;
		if (n < kOP2FaultLog) {
			fFaults[n].far = far;
			fFaults[n].fsr = fsr;
			fFaults[n].fsynr0 = syn;
			fFaults[n].fence = readFenceMem();
		}
		if (fsr & OP2_FSR_MULTI) fFaultMulti++;
		if (syn & OP2_FSYNR0_WNR) fFaultWrites++;
		if (n == 0 || far < fFaultMin) fFaultMin = far;
		if (n == 0 || far > fFaultMax) fFaultMax = far;
	}
	IOLockUnlock(fMapLock);
	return fsr;
}

uint32_t
OP2GPU::checkSMMUFault(const char *where)
{
	uint32_t n0 = fFaultCount;
	uint32_t fsr = pollSMMUFault();
	if (fsr && n0 < kOP2FaultLog) {
		const struct op2_smmu_fault *f = &fFaults[n0];
		IOLog("OP2 GPU SMMU FAULT [%s]: %s 0x%08x FSR=0x%08x (%s%s%s%s%s) FSYNR0=0x%08x\n",
		      where, (f->fsynr0 & OP2_FSYNR0_WNR) ? "write" : "read", f->far, fsr,
		      (fsr & OP2_FSR_TF) ? "translation " : "", (fsr & OP2_FSR_AFF) ? "access-flag " : "",
		      (fsr & OP2_FSR_PF) ? "permission " : "", (fsr & OP2_FSR_EF) ? "external " : "",
		      (fsr & OP2_FSR_MULTI) ? "multiple" : "", f->fsynr0);
	}
	return fsr;
}

bool
OP2GPU::enableTranslation(void)
{
	IOPhysicalAddress phys = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	fPageTableVirt = (uint32_t *)IOMallocContiguous(16384, 16384, &phys);
#pragma clang diagnostic pop
	fL2Virt = (uint32_t **)IOMalloc(4096 * sizeof(uint32_t *));
	fL2Ref = (uint8_t **)IOMalloc(4096 * sizeof(uint8_t *));
	if (!fPageTableVirt || !fL2Virt || !fL2Ref) {
		IOLog("OP2 GPU SMMU: no memory for the page table\n");
		return false;
	}
	fPageTablePhys = (uint64_t)phys;
	bzero(fPageTableVirt, 16384);            /* every 1 MB faults until a table is hooked in */
	flushDcache((vm_offset_t)fPageTableVirt, 16384);
	bzero(fL2Virt, 4096 * sizeof(uint32_t *));
	bzero(fL2Ref, 4096 * sizeof(uint8_t *));

	/* the CP's own memory must be reachable: the ring it fetches and the fence page it writes */
	mapRange(fRingBufferPhys, OP2_GPU_RB_SIZE);
	mapRange(fStatusPagePhys, 4096);

	uint32_t sctlr0 = readCB(OP2_CB_SCTLR);
	if (!programCB0()) {
		IOLog("OP2 GPU SMMU: CB0 did not take our values (SCTLR 0x%08x TTBR0 0x%08x): translation stays OFF\n",
		      readCB(OP2_CB_SCTLR), readCB(OP2_CB_TTBR0));
		writeCB(OP2_CB_SCTLR, sctlr0);
		return false;
	}
	fXlate = true;
	IOLog("OP2 GPU SMMU: translation ON (CB0 SCTLR 0x%08x -> 0x%08x, table phys 0x%llx, 4 KB pages, RAZ/WI on fault); "
	      "ring+fence mapped (%u pages)\n", sctlr0, readCB(OP2_CB_SCTLR), fPageTablePhys, fPagesMapped);
	return true;
}

/* (Re)program CB0 for our table. Returns false if the registers did not take (not NS-writable). */
bool
OP2GPU::programCB0(void)
{
	writeCB(OP2_CB_SCTLR, 0);                 /* bypass while programming (Linux __reset_context) */
	writeCB(OP2_CB_TTBCR, 0);                 /* short descriptor, TTBR0 covers all 4 GB (N=0) */
	writeCB(OP2_CB_TTBR0, (uint32_t)fPageTablePhys);   /* walk attributes 0: non-cacheable (no L2 redirect) */
	writeCB(OP2_CB_TTBR0 + 4, 0);
	writeCB(OP2_CB_TTBR1, 0);
	writeCB(OP2_CB_TTBR1 + 4, 0);
	writeCB(OP2_CB_CONTEXTIDR, 0);            /* ASID 0; TLBIALL does not care */
	writeCB(OP2_CB_FAR, 0);
	writeCB(OP2_CB_FAR + 4, 0);
	writeCB(OP2_CB_FSR, readCB(OP2_CB_FSR));  /* clear stale */
	tlbInvalidateAll();
	writeCB(OP2_CB_SCTLR, OP2_SCTLR_VALUE);
	OSMemoryBarrier();
	return readCB(OP2_CB_SCTLR) == OP2_SCTLR_VALUE &&
	       readCB(OP2_CB_TTBR0) == (uint32_t)fPageTablePhys;
}

/* No ATS (ATS1PR/PAR) helper: any ATS operation on this TZ-configured SMMU resets the SoC (g4d-1..8). */

/*
 * Boot self-test of the translation (gpu_xl=1). Four pages: [0] IB, mapped; [1] victim1, NOT mapped;
 * [2] result, mapped; [3] victim2, NOT mapped, directly after a mapped page (the "write past the end" case).
 *  B1. a write to the mapped result page lands, no fault.
 *  B2. one write to victim1: must latch a translation fault naming its address, and not land.
 *  B3. a write to victim2 (same).
 *  C.  unmap the result page, write it again: must fault and not land (unmap + TLB invalidate take effect).
 * Measured (g4d-9): after a faulting write the CP does NOT go on (RAZ/WI or not): the fence never retires,
 * hang recovery restarts it, CB0 keeps its programming. So each stray write is its own IB, and a timeout
 * after a fault is the expected outcome, not a failure.
 * No ATS (ATS1PR/PAR): on this TZ-configured SMMU any ATS operation resets the SoC a few ms later (g4d-1..8).
 * Lines go straight to ramoops (OP2_SMMU_LOG) so a reset cannot hide where it happened.
 */
void
OP2GPU::selfTestSMMU(void)
{
	if (!fXlate) return;
	IOPhysicalAddress p = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	uint32_t *v = (uint32_t *)IOMallocContiguous(16384, 4096, &p);
#pragma clang diagnostic pop
	if (!v) {
		OP2_SMMU_LOG("OP2 GPU SMMU test: no memory\n");
		return;
	}
	const uint32_t kVictim = 0x5a5a5a5aU;
	uint32_t *ib = v, *victim1 = v + 1024, *res = v + 2048, *victim2 = v + 3072;
	uint32_t pib = (uint32_t)p, pv1 = pib + 4096 + 0x100, pres = pib + 8192, pv2 = pib + 12288 + 0x40;
	for (uint32_t i = 0; i < 1024; i++) {
		victim1[i] = kVictim;
		victim2[i] = kVictim;
		res[i] = 0;
	}
	flushDcache((vm_offset_t)v, 16384);
	mapRange(pib, 4096);
	mapRange(pres, 4096);
	uint32_t hangs0 = fHangs;

	/* one IB per step; returns the number of fault latches it caused, and whether its fence retired */
	auto step = [&](uint32_t addr, uint32_t value, bool *retired) -> uint32_t {
		uint32_t n = 0, f0 = fFaultCount;
		ib[n++] = cp_type3_packet(CP_MEM_WRITE, 2); ib[n++] = addr; ib[n++] = value;
		ib[n++] = cp_type3_packet(CP_NOP, 1);       ib[n++] = 0;
		*retired = runIB(ib, pib, n, 300, NULL);
		pollSMMUFault();
		flushDcache((vm_offset_t)v, 16384);
		return fFaultCount - f0;
	};
	auto named = [&](uint32_t from, uint32_t addr) -> bool {
		for (uint32_t i = from; i < fFaultCount && i < kOP2FaultLog; i++) {
			/* same page: for a just-unmapped page (C) FAR is the page base (g4d-10), elsewhere the exact word */
			if ((fFaults[i].far & ~0xFFFU) == (addr & ~0xFFFU) && (fFaults[i].fsynr0 & OP2_FSYNR0_WNR)) return true;
		}
		return false;
	};

	bool r1 = false, r2 = false, r3 = false, rc = false;
	uint32_t fl1 = step(pres, 0x11111111, &r1);
	bool b1 = r1 && fl1 == 0 && res[0] == 0x11111111;
	OP2_SMMU_LOG("OP2 GPU SMMU test B1: mapped write: fence %s, %u faults, %s\n",
	             r1 ? "retired" : "TIMEOUT", fl1, res[0] == 0x11111111 ? "landed" : "MISSING");

	uint32_t n2 = fFaultCount;
	uint32_t fl2 = step(pv1, 0xdeadbeef, &r2);
	bool b2 = fl2 > 0 && named(n2, pv1) && victim1[0x40] == kVictim;
	OP2_SMMU_LOG("OP2 GPU SMMU test B2: write to unmapped 0x%08x: %u faults%s, victim %s, fence %s\n",
	             pv1, fl2, named(n2, pv1) ? " (FAR = it, write)" : "", victim1[0x40] == kVictim ? "untouched" : "WRITTEN",
	             r2 ? "retired" : "timeout -> recovered");

	uint32_t n3 = fFaultCount;
	uint32_t fl3 = step(pv2, 0xdeadbeef, &r3);
	bool b3 = fl3 > 0 && named(n3, pv2) && victim2[0x10] == kVictim;
	OP2_SMMU_LOG("OP2 GPU SMMU test B3: write 0x40 past a mapped page (0x%08x): %u faults%s, victim %s, fence %s\n",
	             pv2, fl3, named(n3, pv2) ? " (FAR = it, write)" : "", victim2[0x10] == kVictim ? "untouched" : "WRITTEN",
	             r3 ? "retired" : "timeout -> recovered");

	unmapRange(pres, 4096);
	uint32_t nc = fFaultCount;
	uint32_t flc = step(pres + 8, 0x33333333, &rc);
	bool c = flc > 0 && named(nc, pres + 8) && res[2] != 0x33333333;
	OP2_SMMU_LOG("OP2 GPU SMMU test C: write to a just-unmapped page: %u faults%s, %s, fence %s\n",
	             flc, named(nc, pres + 8) ? " (FAR = it)" : "", res[2] == 0x33333333 ? "LANDED" : "dropped",
	             rc ? "retired" : "timeout -> recovered");

	/* the GPU must still work after the recoveries */
	mapRange(pres, 4096);
	bool r4 = false;
	uint32_t fl4 = step(pres + 12, 0x44444444, &r4);
	bool alive = r4 && fl4 == 0 && res[3] == 0x44444444;

	bool contained = b1 && b2 && b3 && c && alive;
	OP2_SMMU_LOG("OP2 GPU SMMU test: %s (B1 %d B2 %d B3 %d C %d after %d; %u recoveries; %u pages mapped, %u L2 tables)\n",
	             contained ? "CONTAINED: stray GPU writes fault at their exact address and are dropped" : "NOT CONTAINED",
	             b1, b2, b3, c, alive, fHangs - hangs0, fPagesMapped, fL2Used);
	setProperty("SMMUContained", contained ? kOSBooleanTrue : kOSBooleanFalse);

	unmapRange(pres, 4096);
	unmapRange(pib, 4096);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	IOFreeContiguous(v, 16384);
#pragma clang diagnostic pop
}

void
OP2GPU::probeSMMU(void)
{
	for (uint32_t cb = 0; cb < 2; cb++) {
		volatile uint32_t *c = (volatile uint32_t *)(fGPUBase + OP2_GPU_SMMU_OFFSET + 0x8000 + cb * 0x1000);
		uint32_t sctlr = c[0x000 / 4];
		uint32_t ttbr0 = c[0x020 / 4];
		uint32_t fsr = c[0x058 / 4];
		uint32_t far = c[0x060 / 4];
		uint32_t fsynr0 = c[0x068 / 4];
		IOLog("OP2 GPU: SMMU CB%u SCTLR=0x%08x (M=%u CFCFG(stall)=%u) TTBR0=0x%08x FSR=0x%08x FAR=0x%08x FSYNR0=0x%08x\n",
		      cb, sctlr, sctlr & 1, (sctlr >> 7) & 1, ttbr0, fsr, far, fsynr0);
	}
}

/*
 * Find out which fence write the CP can actually do into physical memory.
 * Before this, "fence done" was really "ring drained" (RPTR==WPTR).
 */
/*
 * Which GPU writes reach memory? Measured 2026-11-11: CP_MEM_WRITE works, but
 * after one CP_EVENT_WRITE(CACHE_FLUSH_TS) no later memory write ever lands
 * (register writes still do, soft reset does not clear it): the event never
 * retires with the 3D pipe uninitialised. So the event is only sent with
 * gpu_evtest=1, last, and fences are plain CP_MEM_WRITE.
 */
void
OP2GPU::probeFences(void)
{
	volatile uint32_t *st = (volatile uint32_t *)fStatusPageVirt;
	volatile uint32_t *tail = (volatile uint32_t *)&fRingBufferVirt[OP2_GPU_RB_DWORDS - 8];
	uint32_t tail_phys = (uint32_t)fRingBufferPhys + (OP2_GPU_RB_DWORDS - 8) * 4;
	for (int i = 0; i < 8; i++) st[i] = 0;
	tail[0] = 0;
	flushDcache((vm_offset_t)fStatusPageVirt, 64);
	flushDcache((vm_offset_t)tail, 32);
	writeGPU(0x578, 0);   /* CP_SCRATCH_REG0 */

	uint32_t cmds[24];
	uint32_t n = 0;
	cmds[n++] = cp_type3_packet(CP_MEM_WRITE, 2);
	cmds[n++] = (uint32_t)fStatusPagePhys + 4;
	cmds[n++] = 0xc0de0001;
	cmds[n++] = cp_type3_packet(CP_MEM_WRITE, 2);
	cmds[n++] = (uint32_t)fStatusPagePhys + 8;
	cmds[n++] = 0xc0de0002;
	cmds[n++] = (0U << 30) | (0U << 16) | 0x578;     /* type 0: SCRATCH_REG0 */
	cmds[n++] = 0xc0de0003;
	cmds[n++] = cp_type3_packet(CP_WAIT_FOR_IDLE, 1);
	cmds[n++] = 0;
	cmds[n++] = cp_type3_packet(0x3e /* CP_REG_TO_MEM */, 2);
	cmds[n++] = 0x578;
	cmds[n++] = (uint32_t)fStatusPagePhys + 12;
	cmds[n++] = cp_type3_packet(CP_MEM_WRITE, 2);
	cmds[n++] = tail_phys;
	cmds[n++] = 0xc0de0004;
	cmds[n++] = cp_type3_packet(CP_WAIT_FOR_IDLE, 1);
	cmds[n++] = 0;
	submitPacket(cmds, n);
	bool idle = waitIdle(50000);
	IODelay(1000);
	flushDcache((vm_offset_t)fStatusPageVirt, 64);
	flushDcache((vm_offset_t)tail, 32);
	bool mw1 = st[1] == 0xc0de0001, mw2 = st[2] == 0xc0de0002, reg = readGPU(0x578) == 0xc0de0003;
	bool r2m = st[3] == 0xc0de0003, tl = tail[0] == 0xc0de0004;
	IOLog("OP2 GPU: write probe idle=%d MEM_WRITE#1=%s MEM_WRITE#2=%s CP->reg=%s REG_TO_MEM=%s MEM_WRITE->ring=%s "
	      "(0x%08x 0x%08x 0x%08x 0x%08x 0x%08x)\n", idle,
	      mw1 ? "OK" : "FAIL", mw2 ? "OK" : "FAIL", reg ? "OK" : "FAIL", r2m ? "OK" : "FAIL", tl ? "OK" : "FAIL",
	      st[1], st[2], readGPU(0x578), st[3], tail[0]);
	if (!idle) dumpState("write probe");
	fFenceMode = (mw1 && mw2) ? kOP2FenceMemWrite : kOP2FenceRptr;

	uint32_t evtest = 0;
	PE_parse_boot_argn("gpu_evtest", &evtest, sizeof(evtest));
	if (evtest) {
		n = 0;
		cmds[n++] = cp_type3_packet(CP_EVENT_WRITE, 3);
		cmds[n++] = CACHE_FLUSH_TS;
		cmds[n++] = (uint32_t)fStatusPagePhys + 16;
		cmds[n++] = 0xc0de0005;
		cmds[n++] = cp_type3_packet(CP_MEM_WRITE, 2);
		cmds[n++] = (uint32_t)fStatusPagePhys + 20;
		cmds[n++] = 0xc0de0006;
		submitPacket(cmds, n);
		waitIdle(50000);
		IODelay(1000);
		flushDcache((vm_offset_t)fStatusPageVirt, 64);
		IOLog("OP2 GPU: event test: CACHE_FLUSH_TS=0x%08x MEM_WRITE after it=0x%08x\n", st[4], st[5]);
	}
	writeGPU(A4XX_RBBM_AHB_CMD, 1U << 4);   /* clear latched AHB error */
	st[0] = 0;
	flushDcache((vm_offset_t)fStatusPageVirt, 64);
	IOLog("OP2 GPU: fence mode %s\n", fFenceMode == kOP2FenceMemWrite ? "memwrite" : "RPTR (no GPU memory writes!)");
}

bool
OP2GPU::submitIndirectBuffer(uint64_t phys_addr, uint32_t words, uint32_t *out_fence)
{
	if (!fRingBufferVirt || !fHardwareInitialized) return false;

	IOLockLock(fLock);
	uint32_t fence = ++fFenceSeq;
	if (fence == 0) fence = ++fFenceSeq;

	uint32_t cmds[12];
	uint32_t n = 0;
	cmds[n++] = cp_type3_packet(CP_INDIRECT_BUFFER_PFE, 2);
	cmds[n++] = (uint32_t)phys_addr;
	cmds[n++] = words;
	cmds[n++] = cp_type3_packet(CP_WAIT_FOR_IDLE, 1);
	cmds[n++] = 0x00000000;
	n += emitFence(&cmds[n], fence);

	bool ok = submitPacket(cmds, n);
	if (ok) {
		fSubmits++;
		if (out_fence) *out_fence = fence;
	}
	IOLockUnlock(fLock);
	return ok;
}

bool
OP2GPU::isFenceDone(uint32_t fence)
{
	if (fence == 0 || !fStatusPageVirt) return true;
	if (fFenceMode == kOP2FenceRptr) {
		return readGPU(A4XX_CP_RB_RPTR) == fWPTR;
	}
	return (int32_t)(readFenceMem() - fence) >= 0;
}

bool
OP2GPU::waitForFence(uint32_t fence, uint32_t timeout_ms)
{
	uint64_t start = mach_absolute_time();
	uint32_t last_done = readFenceMem();
	uint64_t last_move = start;
	for (uint32_t spin = 0;; spin++) {
		pollSMMUFault();   /* one register read when there is no fault; catches faults while the work runs */
		if (isFenceDone(fence)) {
			pollSMMUFault();
			return true;
		}
		uint64_t now = mach_absolute_time();
		uint32_t done = readFenceMem();
		if (done != last_done) {
			last_done = done;
			last_move = now;
		}
		if (op2_gpu_ticks_to_us(now - start) / 1000ULL >= timeout_ms) {
			break;
		}
		if (spin < 100) {
			IODelay(20);         /* first ~2 ms: spin */
		} else {
			IOSleep(1);
		}
	}
	/*
	 * Hung = no fence retired for >= 100 ms while this one is outstanding.
	 * (RPTR is no use: the PFP prefetches past a stuck IB, so RPTR==WPTR.)
	 */
	bool stuck = (int32_t)(fFenceSeq - readFenceMem()) > 0 &&
	    op2_gpu_ticks_to_us(mach_absolute_time() - last_move) >= 100000ULL;
	if (timeout_ms >= 50 || stuck) {
		IOLog("OP2 GPU: fence %u timeout after %u ms (done=%u seq=%u stuck=%d)\n",
		      fence, timeout_ms, readFenceMem(), fFenceSeq, stuck);
	}
	if (stuck && fFenceMode != kOP2FenceRptr) {
		recover("fence timeout");
	}
	return false;
}

/*
 * GPU hang recovery: soft reset + full restart, then retire every
 * outstanding fence so waiters return (their work is lost).
 */
bool
OP2GPU::recover(const char *why)
{
	IOLockLock(fLock);
	dumpState(why);
	fHangs++;
	bool ok = hwStart();
	if (fXlate && readCB(OP2_CB_SCTLR) != OP2_SCTLR_VALUE) {
		/* the SMMU shares the GPU's power domain: if the reset touched CB0, put our table back */
		uint32_t was = readCB(OP2_CB_SCTLR);
		bool again = programCB0();
		IOLog("OP2 GPU SMMU: CB0 SCTLR was 0x%08x after recovery; reprogrammed: %s\n", was, again ? "ok" : "FAILED");
	}
	if (fStatusPageVirt) {
		((volatile uint32_t *)fStatusPageVirt)[0] = fFenceSeq;
		flushDcache((vm_offset_t)fStatusPageVirt, 64);
	}
	fHardwareInitialized = ok;
	IOLog("OP2 GPU: recovery #%u after '%s': %s (fences retired up to %u)\n",
	      fHangs, why, ok ? "GPU restarted" : "RESTART FAILED", fFenceSeq);
	IOLockUnlock(fLock);
	return ok;
}

/*
 * Boot self-test (gpu_selftest=N, 0 = off):
 *  A. N serial IB submissions; each IB writes its index into a scratch word,
 *     fence waited every time; scratch checked every time.
 *  B. N back-to-back submissions without waiting (wraps the ring), one wait.
 *  C. Hang test (gpu_hangtest=1): IB that waits on memory forever ->
 *     timeout -> recover -> one more normal submission must complete.
 */
void
OP2GPU::selfTest(uint32_t count)
{
	IOPhysicalAddress ib_phys = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	uint32_t *ib = (uint32_t *)IOMallocContiguous(4096, 4096, &ib_phys);
#pragma clang diagnostic pop
	if (!ib) {
		IOLog("OP2 GPU G0: no memory for self-test\n");
		return;
	}
	volatile uint32_t *scratch = (volatile uint32_t *)&ib[512];   /* +2 KB */
	uint32_t scratch_phys = (uint32_t)ib_phys + 2048;
	mapRange(ib_phys, 4096);   /* the GPU reaches it only when translation is on */

	/* A: serial */
	uint32_t failA = 0, badA = 0;
	uint64_t maxA = 0, totA = 0;
	for (uint32_t i = 1; i <= count && failA < 3; i++) {
		ib[0] = cp_type3_packet(CP_MEM_WRITE, 2);
		ib[1] = scratch_phys;
		ib[2] = i;
		ib[3] = cp_type3_packet(CP_NOP, 1);
		ib[4] = 0;
		flushDcache((vm_offset_t)ib, 4096);
		uint32_t fence = 0;
		uint64_t t0 = mach_absolute_time();
		if (!submitIndirectBuffer(ib_phys, 5, &fence) || !waitForFence(fence, 200)) {
			failA++;
			continue;
		}
		uint64_t us = op2_gpu_ticks_to_us(mach_absolute_time() - t0);
		totA += us;
		if (us > maxA) maxA = us;
		flushDcache((vm_offset_t)scratch, 64);
		if (fFenceMode != kOP2FenceRptr && scratch[0] != i) {
			if (badA < 3) IOLog("OP2 GPU G0: serial %u: scratch=%u\n", i, scratch[0]);
			badA++;
		}
	}
	uint32_t doneA = count - failA;

	/* B: burst (also exercises ring wrap: ~9 dwords per submission) */
	ib[0] = cp_type3_packet(CP_NOP, 1);
	ib[1] = 0;
	flushDcache((vm_offset_t)ib, 4096);
	uint32_t failB = 0, last = 0;
	uint64_t t0 = mach_absolute_time();
	for (uint32_t i = 0; i < count; i++) {
		if (!submitIndirectBuffer(ib_phys, 2, &last)) { failB++; break; }
	}
	bool okB = (failB == 0) && waitForFence(last, 2000);
	uint64_t usB = op2_gpu_ticks_to_us(mach_absolute_time() - t0);

	IOLog("OP2 GPU G0: serial %u/%u ok, %u wrong-data, avg %llu us max %llu us; "
	      "burst %u %s in %llu us; mode=%u clock=%u MHz\n",
	      doneA, count, badA, doneA ? totA / doneA : 0, maxA,
	      count, okB ? "ok" : "FAILED", usB, fFenceMode, fCoreMHz);

	/* C: hang + recovery */
	uint32_t hangtest = 1;
	PE_parse_boot_argn("gpu_hangtest", &hangtest, sizeof(hangtest));
	if (hangtest && fFenceMode != kOP2FenceRptr) {
		scratch[1] = 0;
		flushDcache((vm_offset_t)scratch, 64);
		ib[0] = cp_type3_packet(CP_WAIT_REG_MEM, 5);
		ib[1] = 3 | (1U << 4);        /* function: equal, memory space */
		ib[2] = scratch_phys + 4;
		ib[3] = 0x12345678;           /* never written */
		ib[4] = 0xffffffff;
		ib[5] = 16;                   /* poll interval */
		flushDcache((vm_offset_t)ib, 4096);
		uint32_t hf = 0;
		uint32_t hangs0 = fHangs;
		submitIndirectBuffer(ib_phys, 6, &hf);
		bool hung = !waitForFence(hf, 300);
		/* after recovery: one normal submission must complete */
		ib[0] = cp_type3_packet(CP_MEM_WRITE, 2);
		ib[1] = scratch_phys;
		ib[2] = 0xbeef0001;
		ib[3] = cp_type3_packet(CP_NOP, 1);
		ib[4] = 0;
		flushDcache((vm_offset_t)ib, 4096);
		uint32_t pf = 0;
		bool after = submitIndirectBuffer(ib_phys, 5, &pf) && waitForFence(pf, 200);
		flushDcache((vm_offset_t)scratch, 64);
		IOLog("OP2 GPU G0: hang test: hung=%d recovered=%d post-recovery submit=%s (scratch=0x%08x)\n",
		      hung, fHangs > hangs0, (after && scratch[0] == 0xbeef0001) ? "ok" : "FAILED",
		      scratch[0]);
	}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	unmapRange(ib_phys, 4096);
	IOFreeContiguous(ib, 4096);
#pragma clang diagnostic pop
}

/*
 * G1 self-test: CP-only fill/copy primitives on A4xx (no CP_MEMCPY before A6xx).
 * Each step is its own IB + fence; an unknown opcode shows as CP_OPCODE_ERROR
 * (RBBM_INT_0 bit 9) or a hang, which recovery handles.
 */
bool
OP2GPU::runIB(uint32_t *ib, uint64_t ib_phys, uint32_t words, uint32_t timeout_ms, uint64_t *us_out)
{
	flushDcache((vm_offset_t)ib, words * 4);
	writeGPU(A4XX_RBBM_INT_CLEAR_CMD, 0xffffffff);
	uint32_t f = 0;
	uint64_t t0 = mach_absolute_time();
	bool ok = submitIndirectBuffer(ib_phys, words, &f) && waitForFence(f, timeout_ms);
	if (us_out) *us_out = op2_gpu_ticks_to_us(mach_absolute_time() - t0);
	return ok;
}

void
OP2GPU::selfTestG1(void)
{
	const uint32_t kIB = 0, kSrc = 0x10000, kDst = 0x20000, kSize = 0x30000;   /* bytes */
	IOPhysicalAddress phys = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	uint32_t *mem = (uint32_t *)IOMallocContiguous(kSize + 0x10000, 4096, &phys);
#pragma clang diagnostic pop
	if (!mem) { IOLog("OP2 GPU G1: no memory\n"); return; }
	uint32_t *ib = mem + kIB / 4;
	volatile uint32_t *src = mem + kSrc / 4;
	volatile uint32_t *dst = mem + kDst / 4;
	uint32_t pib = (uint32_t)phys + kIB, psrc = (uint32_t)phys + kSrc, pdst = (uint32_t)phys + kDst;
	mapRange(phys, kSize + 0x10000);

	auto reset = [&]() {
		for (uint32_t i = 0; i < 0x4000; i++) { src[i] = 0x51000000u | i; dst[i] = 0xdddddddd; }
		flushDcache((vm_offset_t)(mem + kSrc / 4), 0x20000);
	};
	auto inval = [&]() { flushDcache((vm_offset_t)(mem + kSrc / 4), 0x20000); };

	/* 1. fill: CP_MEM_WRITE with 256 inline words */
	reset();
	uint32_t n = 0;
	ib[n++] = cp_type3_packet(CP_MEM_WRITE, 257);
	ib[n++] = pdst;
	for (uint32_t i = 0; i < 256; i++) ib[n++] = (i * 0x01010101u) ^ 0xa5a5a5a5u;
	bool ok1 = runIB(ib, pib, n, 100, NULL);
	inval();
	uint32_t bad1 = 0;
	for (uint32_t i = 0; i < 256; i++) if (dst[i] != ((i * 0x01010101u) ^ 0xa5a5a5a5u)) bad1++;
	bool canary1 = dst[256] == 0xdddddddd;
	uint32_t int1 = readGPU(A4XX_RBBM_INT_0_STATUS);

	/* 2. CP_MEM_TO_MEM (0x73): dst = srcA, one word */
	reset();
	n = 0;
	ib[n++] = cp_type3_packet(0x73, 3);
	ib[n++] = 0;               /* flags: 32-bit, no negate */
	ib[n++] = pdst + 0x400;
	ib[n++] = psrc + 0x40;
	bool ok2 = runIB(ib, pib, n, 100, NULL);
	inval();
	uint32_t v2 = dst[0x100];
	bool good2 = (v2 == src[0x10]) && dst[0x101] == 0xdddddddd;
	uint32_t int2 = readGPU(A4XX_RBBM_INT_0_STATUS);

	/* 3. CP_MEM_TO_MEM 64-bit (DOUBLE, bit 29), then a 4096-word copy as 2048 such packets */
	reset();
	n = 0;
	for (uint32_t i = 0; i < 2048; i++) {
		ib[n++] = cp_type3_packet(0x73, 3);
		ib[n++] = 1U << 29;
		ib[n++] = pdst + 0x4000 + i * 8;
		ib[n++] = psrc + i * 8;
	}
	uint64_t us3 = 0;
	bool ok3 = runIB(ib, pib, n, 200, &us3);
	inval();
	uint32_t bad3 = 0;
	for (uint32_t i = 0; i < 4096; i++) if (dst[0x1000 + i] != src[i]) bad3++;
	bool canary3 = dst[0x1000 + 4096] == 0xdddddddd && dst[0xfff] == 0xdddddddd;
	uint32_t int3 = readGPU(A4XX_RBBM_INT_0_STATUS);

	/* 4. throughput: one 16000-word inline fill */
	reset();
	n = 0;
	ib[n++] = cp_type3_packet(CP_MEM_WRITE, 16001);
	ib[n++] = pdst;
	for (uint32_t i = 0; i < 16000; i++) ib[n++] = 0x0f000000u | i;
	uint64_t us4 = 0;
	bool ok4 = runIB(ib, pib, n, 200, &us4);
	inval();
	uint32_t bad4 = 0;
	for (uint32_t i = 0; i < 16000; i++) if (dst[i] != (0x0f000000u | i)) bad4++;

	IOLog("OP2 GPU G1: fill256 %s (%u wrong, canary %s, int 0x%x); MEM_TO_MEM %s (got 0x%08x want 0x%08x, int 0x%x); "
	      "copy4096 via MEM_TO_MEM64 %s (%u wrong, canary %s, int 0x%x) in %llu us; fill16000 %s %u wrong in %llu us\n",
	      ok1 ? "done" : "TIMEOUT", bad1, canary1 ? "ok" : "HIT", int1,
	      ok2 ? (good2 ? "ok" : "WRONG") : "TIMEOUT", v2, src[0x10], int2,
	      ok3 ? "done" : "TIMEOUT", bad3, canary3 ? "ok" : "HIT", int3, us3,
	      ok4 ? "done" : "TIMEOUT", bad4, us4);

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	unmapRange(phys, kSize + 0x10000);
	IOFreeContiguous(mem, kSize + 0x10000);
#pragma clang diagnostic pop
}

bool
OP2GPU::init(OSDictionary *dictionary)
{
	if (!IOService::init(dictionary))
		return false;

	fMMSSBase = 0;
	fGPUBase = 0;
	fRingBufferVirt = NULL;
	fRingBufferPhys = 0;
	fWPTR = 0;
	fStatusPageVirt = NULL;
	fStatusPagePhys = 0;
	fFenceSeq = 0;
	fFenceMode = kOP2FenceRptr;
	fHangs = 0;
	fSubmits = 0;
	fCoreMHz = 0;
	fPageTableVirt = NULL;
	fPageTablePhys = 0;
	fL2Virt = NULL;
	fL2Ref = NULL;
	fL2Used = 0;
	fPagesMapped = 0;
	fMapLock = IOLockAlloc();
	fXlate = false;
	bzero(fFaults, sizeof(fFaults));
	fFaultCount = fFaultMulti = fFaultWrites = 0;
	fFaultMin = fFaultMax = 0;
	fEnabled = false;
	fHardwareInitialized = false;
	fLock = IOLockAlloc();

	return true;
}

/* G5b: the held list is shared by every client (backboardd and SpringBoard close buffers at the same time:
 * round g5b-2 released one descriptor twice, and a user client's mappings set held the freed map). Each client's
 * fLock does not cover it; this does. Taken inside fLock, never the other way round. */
static IOLock *gHeldLock;

/*
 * G5b: a physically contiguous pool reserved at boot for large GPU buffers. Once iOS is up, 28 MB of contiguous
 * memory is not to be had (round g5b-9: CA's 2592x2592 BGRA texture was refused, and what it held drew black);
 * at boot it is. Made only with metal=2 or more (CoreAnimation on the GPU); buffers of OP2_GPU_POOL_MIN or more
 * are carved from it (first fit, 64 KB granules), the rest, and anything the pool cannot fit, as before.
 */
#define OP2_GPU_POOL_BYTES   (192ULL << 20)
#define OP2_GPU_POOL_BYTES3  (512ULL << 20)   /* metal=3: backboardd AND SpringBoard render big targets (fable-25), and
                                                * every IOSurface comes from the pool too (fable-30) */
static uint64_t gPoolBytes = OP2_GPU_POOL_BYTES;
#define OP2_GPU_POOL_MIN     (2ULL << 20)
#define OP2_GPU_POOL_GRANULE (64ULL << 10)
#define OP2_GPU_POOL_EXTENTS 128
static IOBufferMemoryDescriptor *gPool;
static uint8_t *gPoolKaddr;
static uint64_t gPoolPhys;
static struct { uint64_t off, len; } gPoolFree[OP2_GPU_POOL_EXTENTS];   /* sorted by off, merged */
static unsigned gPoolNFree;
static IOLock *gPoolLock;

static void
op2_pool_init(uint32_t metal_level)
{
	gPoolBytes = metal_level >= 3 ? OP2_GPU_POOL_BYTES3 : OP2_GPU_POOL_BYTES;
	gPoolLock = IOLockAlloc();
	gPool = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
	    kIODirectionInOut | kIOMemoryKernelUserShared | kIOMemoryPhysicallyContiguous,
	    (vm_size_t)gPoolBytes, 0x00000000FFFFF000ULL);
	if (!gPool || !gPoolLock) {
		IOLog("OP2 GPU POOL: no %llu MB contiguous below 4 GB at boot: large buffers allocate as before\n",
		      gPoolBytes >> 20);
		OSSafeReleaseNULL(gPool);
		return;
	}
	gPoolKaddr = (uint8_t *)gPool->getBytesNoCopy();
	gPoolPhys = (uint64_t)gPool->getPhysicalAddress();
	gPoolFree[0].off = 0;
	gPoolFree[0].len = gPoolBytes;
	gPoolNFree = 1;
	IOLog("OP2 GPU POOL: %llu MB at phys 0x%llx for buffers of %llu MB or more\n", gPoolBytes >> 20,
	      gPoolPhys, OP2_GPU_POOL_MIN >> 20);
}

static bool
op2_pool_alloc(uint64_t len, uint64_t *off)
{
	bool ok = false;
	IOLockLock(gPoolLock);
	for (unsigned i = 0; i < gPoolNFree; i++) {
		if (gPoolFree[i].len >= len) {
			*off = gPoolFree[i].off;
			gPoolFree[i].off += len;
			gPoolFree[i].len -= len;
			if (!gPoolFree[i].len) {
				memmove(&gPoolFree[i], &gPoolFree[i + 1], (gPoolNFree - i - 1) * sizeof(gPoolFree[0]));
				gPoolNFree--;
			}
			ok = true;
			break;
		}
	}
	IOLockUnlock(gPoolLock);
	return ok;
}

static void
op2_pool_free(uint64_t off, uint64_t len)
{
	IOLockLock(gPoolLock);
	unsigned i = 0;
	while (i < gPoolNFree && gPoolFree[i].off < off)
		i++;
	bool prev = i > 0 && gPoolFree[i - 1].off + gPoolFree[i - 1].len == off;
	bool next = i < gPoolNFree && off + len == gPoolFree[i].off;
	if (prev && next) {
		gPoolFree[i - 1].len += len + gPoolFree[i].len;
		memmove(&gPoolFree[i], &gPoolFree[i + 1], (gPoolNFree - i - 1) * sizeof(gPoolFree[0]));
		gPoolNFree--;
	} else if (prev) {
		gPoolFree[i - 1].len += len;
	} else if (next) {
		gPoolFree[i].off = off;
		gPoolFree[i].len += len;
	} else if (gPoolNFree < OP2_GPU_POOL_EXTENTS) {
		memmove(&gPoolFree[i + 1], &gPoolFree[i], (gPoolNFree - i) * sizeof(gPoolFree[0]));
		gPoolFree[i].off = off;
		gPoolFree[i].len = len;
		gPoolNFree++;
	} else {
		IOLog("OP2 GPU POOL: free list full: %llu bytes at +0x%llx lost\n", len, off);
	}
	IOLockUnlock(gPoolLock);
}

/*
 * IOSurfaces from the pool (OP2Surface, metal>=2). Round fable-30: SpringBoard made new 11 MB surfaces long after boot,
 * contiguous memory had run out ("vm_page_find_contiguous ... found 0"), the surfaces were scattered, the GPU wrote them
 * as if contiguous: SMMU faults, three GPU hangs, SpringBoard crashed. The pool was reserved at boot and stays
 * contiguous. take: a sub-descriptor and kernel address of `bytes` (granule-rounded); give: the range back.
 */
extern "C" bool
op2_gpu_pool_take(uint64_t bytes, IOMemoryDescriptor **desc, void **kaddr, uint64_t *off, uint64_t *len)
{
	if (!gPool || !gPoolLock)
		return false;
	uint64_t l = (bytes + OP2_GPU_POOL_GRANULE - 1) & ~(OP2_GPU_POOL_GRANULE - 1);
	uint64_t o = 0;
	if (!op2_pool_alloc(l, &o))
		return false;
	IOMemoryDescriptor *d = IOSubMemoryDescriptor::withSubRange(gPool, (IOByteCount)o, (IOByteCount)l,
	                                                             kIODirectionInOut);
	if (!d) {
		op2_pool_free(o, l);
		return false;
	}
	*desc = d;
	*kaddr = gPoolKaddr + o;
	*off = o;
	*len = l;
	return true;
}

extern "C" void
op2_gpu_pool_give(uint64_t off, uint64_t len)
{
	if (gPoolLock && len)
		op2_pool_free(off, len);
}

bool
OP2GPU::start(IOService *provider)
{
	if (!IOService::start(provider))
		return false;
	if (!gHeldLock)
		gHeldLock = IOLockAlloc();
	{
		uint32_t metal_level = 0;
		if (!gPool && PE_parse_boot_argn("metal", &metal_level, sizeof(metal_level)) && metal_level >= 2)
			op2_pool_init(metal_level);
	}

	uint32_t gpu_arg = 1;
	PE_parse_boot_argn("gpu", &gpu_arg, sizeof(gpu_arg));
	if (gpu_arg == 0) {
		IOLog("OP2 GPU: driver disabled (gpu=0)\n");
		registerService();
		return true;
	}

	uint32_t gpu_hw = 1;
	PE_parse_boot_argn("gpu_hw", &gpu_hw, sizeof(gpu_hw));
	uint32_t gpu_khz = 300000;        /* RPM vote: without it the core clock is off (0 cycles, VBIF/UCHE writes hang) */
	PE_parse_boot_argn("gpu_khz", &gpu_khz, sizeof(gpu_khz));
	uint32_t gpu_selftest = 1000;
	PE_parse_boot_argn("gpu_selftest", &gpu_selftest, sizeof(gpu_selftest));
	uint32_t gpu_smmu = 1;   /* read SMMU context banks (global space reset the SoC, r6) */
	PE_parse_boot_argn("gpu_smmu", &gpu_smmu, sizeof(gpu_smmu));

	IOLog("OP2 GPU: Adreno 430 (gpu_hw=%u gpu_khz=%u selftest=%u)\n",
	      gpu_hw, gpu_khz, gpu_selftest);

	setProperty("DeviceName", "Qualcomm Adreno 430");
	setProperty("ChipID", 0x04030002, 32);
	setProperty("MetalPluginName", "OP2Metal");
	setProperty("MetalPluginClassName", "OP2MetalDevice");
	setProperty("IOGLBundleName", "OP2Metal");
	setProperty("IOGLPluginName", "OP2Metal");
	setProperty("IOGLESBundleName", "AGXGLDriver");
	setProperty("IOGLESMetalBundleName", "AGXGLDriver");
	setProperty("RingBufferSize", OP2_GPU_RB_SIZE, 32);
	setProperty(kIOUserClientClassKey, "OP2GPUUserClient");

	gOP2GPUInstance = this;

	if (gpu_hw == 0) {
		setProperty("HardwareAccelerated", kOSBooleanFalse);
		registerService();
		return true;
	}

	fMMSSBase = ml_io_map(OP2_MMSSCC_PHYS_BASE, OP2_MMSSCC_PHYS_SIZE);
	fGPUBase = ml_io_map(OP2_ADRENO_PHYS_BASE, OP2_ADRENO_PHYS_SIZE);
	if (!fMMSSBase || !fGPUBase) {
		IOLog("OP2 GPU: ERROR: MMIO map failed\n");
		registerService();
		return true;
	}

	if (!powerOnClocks()) {
		IOLog("OP2 GPU: powerOnClocks failed; not touching Adreno MMIO\n");
		setProperty("HardwareAccelerated", kOSBooleanFalse);
		registerService();
		return true;
	}

	if (!allocRingBuffer()) {
		registerService();
		return true;
	}

	PE_parse_boot_argn("gpu_init", &gOP2GPUInitSteps, sizeof(gOP2GPUInitSteps));
	/* KGSL sets the core clock before it touches the core: vote first */
	if (gpu_khz) {
		voteCoreClock(gpu_khz);
		IODelay(1000);
	}
	/* GPU SMMU is TZ-owned (secure id 18). KGSL: scm_restore_sec_cfg per
	 * context at attach, before the ringbuffer starts. gpu_smmusec=0 skips. */
	uint32_t gpu_smmusec = 1;
	PE_parse_boot_argn("gpu_smmusec", &gpu_smmusec, sizeof(gpu_smmusec));
	if (gpu_smmusec) {
		bool sec = restoreSMMUSecureConfig();
		setProperty("SMMUSecureConfig", sec ? kOSBooleanTrue : kOSBooleanFalse);
	}

	/* Put the GPU behind real translation so a stray write faults instead of corrupting RAM
	 * (G4c). Off by default during bring-up; gpu_xl=1 turns it on (short: the cmdline is full), and so does
	 * metal=2 or more (G5: CoreAnimation drawing through the GPU), which has no room left for gpu_xl=1;
	 * gpu_xl=0 still turns it off. */
	uint32_t gpu_smmu_xlate = 0, metal_level = 0;
	if (PE_parse_boot_argn("metal", &metal_level, sizeof(metal_level)) && metal_level >= 2)
		gpu_smmu_xlate = 1;
	PE_parse_boot_argn("gpu_xl", &gpu_smmu_xlate, sizeof(gpu_smmu_xlate));
	if (gpu_smmu_xlate && gpu_smmusec) {
		enableTranslation();
		setProperty("SMMUTranslation", fXlate ? kOSBooleanTrue : kOSBooleanFalse);
	}

	if (!hwStart()) {
		IOLog("OP2 GPU: hardware start failed\n");
		setProperty("HardwareAccelerated", kOSBooleanFalse);
		registerService();
		return true;
	}
	IOLog("OP2 GPU: CP running (ME_INIT idle, RPTR=%u)\n", readGPU(A4XX_CP_RB_RPTR));

	fCoreMHz = measureCoreMHz();
	if (gpu_smmu) {
		probeSMMU();
	}
	probeFences();
	if (gpu_smmu) {
		probeSMMU();
	}
	fHardwareInitialized = true;

	selfTestSMMU();   /* only with translation on */
	if (gpu_selftest) {
		selfTest(gpu_selftest);
		selfTestG1();
	}
	if (fXlate) {
		checkSMMUFault("after self-tests");
		IOLog("OP2 GPU SMMU: after self-tests: %u fault latches in all, %u pages still mapped\n",
		      fFaultCount, fPagesMapped);
	}
	setProperty("GPUCoreMHz", fCoreMHz, 32);
	setProperty("FenceMode", fFenceMode, 32);
	setProperty("HardwareAccelerated", fHardwareInitialized ? kOSBooleanTrue : kOSBooleanFalse);
	registerService();
	IOLog("OP2 GPU: ready=%d fence-mode=%u hangs=%u\n", fHardwareInitialized, fFenceMode, fHangs);
	return true;
}

void
OP2GPU::stop(IOService *provider)
{
	if (gOP2GPUInstance == this) {
		gOP2GPUInstance = NULL;
	}
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	if (fRingBufferVirt) {
		IOFreeContiguous(fRingBufferVirt, OP2_GPU_RB_SIZE);
		fRingBufferVirt = NULL;
	}
	if (fXlate) {
		writeCB(OP2_CB_SCTLR, 0);   /* back to bypass before the table goes away */
		fXlate = false;
	}
	if (fPageTableVirt) {
		IOFreeContiguous(fPageTableVirt, 16384);
		fPageTableVirt = NULL;
	}
	for (uint32_t c = 0; c < OP2_L2_MAX_CHUNKS; c++) {
		if (gL2ChunkVirt[c]) {
			IOFreeContiguous(gL2ChunkVirt[c], 16384);
			gL2ChunkVirt[c] = NULL;
		}
	}
	if (fL2Ref) {
		for (uint32_t s = 0; s < 4096; s++) {
			if (fL2Ref[s]) IOFree(fL2Ref[s], 256);
		}
		IOFree(fL2Ref, 4096 * sizeof(uint8_t *));
		fL2Ref = NULL;
	}
	if (fL2Virt) {
		IOFree(fL2Virt, 4096 * sizeof(uint32_t *));
		fL2Virt = NULL;
	}
	if (fStatusPageVirt) {
		IOFreeContiguous(fStatusPageVirt, 4096);
		fStatusPageVirt = NULL;
	}
#pragma clang diagnostic pop
	if (fLock) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	if (fMapLock) {
		IOLockFree(fMapLock);
		fMapLock = NULL;
	}
	IOService::stop(provider);
}

IOReturn
OP2GPU::newUserClient(task_t owningTask, void *securityID,
    UInt32 type, OSDictionary *properties,
    IOUserClient **handler)
{
	if (!handler) return kIOReturnBadArgument;

	OP2GPUUserClient *client = OSTypeAlloc(OP2GPUUserClient);
	if (!client) {
		return kIOReturnNoMemory;
	}

	if (!client->initWithTask(owningTask, securityID, type, properties)) {
		client->release();
		return kIOReturnError;
	}

	if (!client->attach(this)) {
		client->release();
		return kIOReturnError;
	}

	if (!client->start(this)) {
		client->detach(this);
		client->release();
		return kIOReturnError;
	}

	*handler = client;
	IOLog("OP2 GPU: new user client connected (task=%p type=0x%x)\n", owningTask, type);
	return kIOReturnSuccess;
}

IOReturn
OP2GPU::newUserClient(task_t owningTask, void *securityID,
    UInt32 type, IOUserClient **handler)
{
	return newUserClient(owningTask, securityID, type, NULL, handler);
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
/* OP2GPUUserClient Implementation                                       */
/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

static IOReturn
op2_gpu_struct_out(IOExternalMethodArguments *args, const void *buf, uint32_t len)
{
	if (args->structureOutputDescriptor != NULL) {
		uint32_t copy_len = len;
		if (copy_len > args->structureOutputDescriptor->getLength()) {
			copy_len = (uint32_t)args->structureOutputDescriptor->getLength();
		}
		args->structureOutputDescriptor->writeBytes(0, buf, copy_len);
		args->structureOutputDescriptorSize = copy_len;
		return kIOReturnSuccess;
	}
	if (args->structureOutput == NULL) {
		return kIOReturnBadArgument;
	}
	uint32_t copy_len = len;
	if (copy_len > args->structureOutputSize) {
		copy_len = args->structureOutputSize;
	}
	memcpy(args->structureOutput, buf, copy_len);
	args->structureOutputSize = copy_len;
	return kIOReturnSuccess;
}

/*
 * msm DRM semantics (and hakios-metal's gpu.cpp): the timeout is an absolute
 * deadline in ns of mach_continuous_time. Returns the ms left (0 = poll once).
 */
static uint32_t
op2_gpu_deadline_ms(const struct msm_timespec *t)
{
	int64_t deadline = t->seconds * 1000000000LL + t->nanoseconds;
	uint64_t now_ns = 0;
	absolutetime_to_nanoseconds(mach_continuous_time(), &now_ns);
	int64_t left = deadline - (int64_t)now_ns;
	if (left <= 0) return 0;
	if (left > 60000000000LL) left = 60000000000LL;   /* cap at 60 s */
	return (uint32_t)(left / 1000000LL);
}

/*
 * Buffer-lifetime debugging (G4b, 2026-11-11). After a large draw, other processes crashed with
 * corrupted state, which suggests the GPU wrote memory it no longer owned. Two checks:
 *  - every gemNew buffer gets OP2_GPU_GUARD bytes after its end filled with kGuardWord; they are
 *    compared when the buffer is closed (a draw that ran past the end shows up there);
 *  - a closed buffer is not returned to the system at once: it is filled with kHeldWord and kept on
 *    a list (up to OP2_GPU_HELD_BYTES); it is compared again before its memory is returned and at every
 *    client exit (a write that arrives after close shows up there).
 * boot-arg gpu_held=0 turns the second off.
 */
#define OP2_GPU_GUARD       16384ULL
#define OP2_GPU_HELD_BYTES  (48ULL << 20)
#define OP2_GPU_HELD_MAX    64
static const uint32_t kGuardWord = 0xa5a5a5a5;
static const uint32_t kHeldWord = 0xc5c5c5c5;

struct op2_held {
	IOMemoryDescriptor *desc;
	uint32_t *kaddr;
	uint64_t phys, bytes;
	uint32_t handle, seq;
};
static struct op2_held gHeld[OP2_GPU_HELD_MAX];
static unsigned gHeldCount;
static uint64_t gHeldBytes;
static uint32_t gHeldSeq;
static uint32_t gReports;

/* first word of [kaddr, kaddr+bytes) that is not `want`; the number that differ in *count */
static long
op2_first_diff(uint32_t *kaddr, uint64_t bytes, uint32_t want, uint32_t *count)
{
	flushDcache((vm_offset_t)kaddr, (vm_size_t)bytes);
	long first = -1;
	*count = 0;
	for (uint64_t i = 0; i < bytes / 4; i++) {
		if (kaddr[i] != want) {
			if (first < 0) first = (long)i;
			(*count)++;
		}
	}
	return first;
}

static void
op2_fill(uint32_t *kaddr, uint64_t bytes, uint32_t word)
{
	for (uint64_t i = 0; i < bytes / 4; i++) kaddr[i] = word;
	flushDcache((vm_offset_t)kaddr, (vm_size_t)bytes);
}

static void
op2_held_check(struct op2_held *h, const char *when)
{
	uint32_t n = 0;
	long first = op2_first_diff(h->kaddr, h->bytes, kHeldWord, &n);
	if (first >= 0) {
		if (gReports++ < 40) {
			IOLog("OP2 GPU HELD: buffer %u (phys 0x%llx, %llu bytes, closed #%u) CHANGED after close (%s): "
			      "%u words differ, first at +0x%lx = 0x%08x\n",
			      h->handle, h->phys, h->bytes, h->seq, when, n, first * 4, h->kaddr[first]);
		}
		op2_fill(h->kaddr, h->bytes, kHeldWord);   /* report the next change as new */
	}
}

/* every held buffer checked (client exit) */
static void
op2_held_sweep(const char *when)
{
	for (unsigned i = 0; i < gHeldCount; i++) op2_held_check(&gHeld[i], when);
}

static void
op2_held_add(IOMemoryDescriptor *desc, uint32_t *kaddr, uint64_t phys, uint64_t bytes, uint32_t handle)
{
	while (gHeldCount && (gHeldCount >= OP2_GPU_HELD_MAX || gHeldBytes + bytes > OP2_GPU_HELD_BYTES)) {
		struct op2_held *old = &gHeld[0];
		op2_held_check(old, "before release");
		old->desc->release();
		gHeldBytes -= old->bytes;
		memmove(&gHeld[0], &gHeld[1], (gHeldCount - 1) * sizeof(gHeld[0]));
		gHeldCount--;
	}
	op2_fill(kaddr, bytes, kHeldWord);
	gHeld[gHeldCount++] = (struct op2_held){ desc, kaddr, phys, bytes, handle, ++gHeldSeq };
	gHeldBytes += bytes;
}

void
OP2GPUUserClient::releaseBuffer(struct op2_gpu_client_buffer *b, uint32_t handle)
{
	bool done = true;
	if (b->fence != 0 && fOwner) {
		done = fOwner->waitForFence(b->fence, 1000);
		if (!done && gReports++ < 40) {
			IOLog("OP2 GPU HELD: buffer %u closed with fence %u still pending\n", handle, b->fence);
		}
	}
	/* after the fence: the GPU is done with it. Unmapped either way, so a late GPU access faults instead of
	 * landing in memory that is about to be reused. (Owned buffers are contiguous; surfaces go by segment.) */
	if (fOwner && b->size) {
		if (b->desc && !b->owned) fOwner->mapDescriptor(b->desc, b->size, false);
		else if (b->phys_addr) fOwner->unmapRange(b->phys_addr, b->size);
	}
	if (b->desc) {
		/* G5b: off by default once CA draws on the GPU (metal=2+): the G4b stray write it hunted is fixed, and
		 * 48 MB of held contiguous memory is what large textures then cannot get */
		uint32_t held = 1, metal_level = 0;
		if (PE_parse_boot_argn("metal", &metal_level, sizeof(metal_level)) && metal_level >= 2)
			held = 0;
		PE_parse_boot_argn("gpu_held", &held, sizeof(held));
		if (b->owned) {
			uint32_t n = 0;
			uint32_t *guard = (uint32_t *)b->kernel_addr + b->size / 4;
			long first = op2_first_diff(guard, OP2_GPU_GUARD, kGuardWord, &n);
			if (first >= 0 && gReports++ < 40) {
				IOLog("OP2 GPU GUARD: buffer %u (phys 0x%llx, %llu bytes) written PAST ITS END: %u words of the "
				      "guard differ, first at end+0x%lx = 0x%08x\n",
				      handle, b->phys_addr, b->size, n, first * 4, guard[first]);
			}
		}
		if (b->pool_len) {
			/* from the pool: back to it (never held: the pool is what large buffers need) */
			b->desc->release();
			op2_pool_free(b->pool_off, b->pool_len);
		} else if (b->owned && held && gHeldLock) {
			IOLockLock(gHeldLock);
			op2_held_add(b->desc, (uint32_t *)b->kernel_addr, b->phys_addr, b->alloc_size, handle);
			IOLockUnlock(gHeldLock);
		} else {
			b->desc->release();
		}
		b->desc = NULL;
	}
	b->kernel_addr = NULL;
	b->phys_addr = 0;
	b->size = 0;
	b->alloc_size = 0;
	b->pool_off = 0;
	b->pool_len = 0;
	b->user_mapped = false;
	b->fence = 0;
	b->owned = false;
	b->in_use = false;
}

bool
OP2GPUUserClient::initWithTask(task_t owningTask, void *securityID,
    UInt32 type, OSDictionary *properties)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type, properties)) {
		return false;
	}
	fTask = owningTask;
	fOwner = NULL;
	fBufferCount = 0;
	fFaultsSeen = 0;
	fLock = IOLockAlloc();
	fBuffers = (op2_gpu_client_buffer *)IOMalloc(OP2_GPU_MAX_BUFFERS * sizeof(op2_gpu_client_buffer));
	if (!fLock || !fBuffers)
		return false;
	bzero(fBuffers, OP2_GPU_MAX_BUFFERS * sizeof(op2_gpu_client_buffer));
	return true;
}

bool
OP2GPUUserClient::start(IOService *provider)
{
	if (!IOUserClient::start(provider)) {
		return false;
	}
	fOwner = OSDynamicCast(OP2GPU, provider);
	if (!fOwner) {
		return false;
	}
	fFaultsSeen = fOwner->fFaultCount;   /* describe only faults from our own time, not the boot self-test's */
	return true;
}

void
OP2GPUUserClient::stop(IOService *provider)
{
	IOUserClient::stop(provider);
}

IOReturn
OP2GPUUserClient::clientClose(void)
{
	describeFaults("client exit");
	IOLockLock(fLock);
	for (uint32_t i = 0; i < OP2_GPU_MAX_BUFFERS; i++) {
		if (fBuffers[i].in_use) {
			releaseBuffer(&fBuffers[i], i + 1);
		}
	}
	fBufferCount = 0;
	if (gHeldLock) {
		IOLockLock(gHeldLock);
		op2_held_sweep("client exit");
		IOLog("OP2 GPU HELD: client exit: %u buffers (%llu bytes) held, %u reports so far\n",
		      gHeldCount, gHeldBytes, gReports);
		IOLockUnlock(gHeldLock);
	}
	IOLockUnlock(fLock);

	terminate();
	return kIOReturnSuccess;
}

void
OP2GPUUserClient::free(void)
{
	if (fLock) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	if (fBuffers) {
		IOFree(fBuffers, OP2_GPU_MAX_BUFFERS * sizeof(op2_gpu_client_buffer));
		fBuffers = NULL;
	}
	IOUserClient::free();
}

struct op2_gpu_client_buffer *
OP2GPUUserClient::getBuffer(uint32_t handle)
{
	if (handle == 0 || handle > OP2_GPU_MAX_BUFFERS) {
		return NULL;
	}
	uint32_t idx = handle - 1;
	if (!fBuffers[idx].in_use) {
		return NULL;
	}
	return &fBuffers[idx];
}

IOReturn
OP2GPUUserClient::gemNew(struct msm_gem_new *req)
{
	if (req->size == 0 || req->size > (64ULL << 20)) {
		return kIOReturnBadArgument;
	}

	uint64_t aligned_size = (req->size + 4095ULL) & ~4095ULL;

	IOLockLock(fLock);

	int slot = -1;
	for (uint32_t i = 0; i < OP2_GPU_MAX_BUFFERS; i++) {
		if (!fBuffers[i].in_use) {
			slot = (int)i;
			break;
		}
	}
	static uint32_t refusals;
	if (slot == -1) {
		IOLockUnlock(fLock);
		if (refusals++ < 20)
			IOLog("OP2 GPU Client: gemNew %llu bytes refused: all %u buffer slots in use\n", req->size,
			      OP2_GPU_MAX_BUFFERS);
		return kIOReturnNoSpace;
	}

	IOMemoryDescriptor *desc = NULL;
	void *kaddr = NULL;
	uint64_t phys = 0, pool_off = 0, pool_len = 0;
	if (gPool && aligned_size >= OP2_GPU_POOL_MIN) {
		uint64_t len = (aligned_size + OP2_GPU_GUARD + OP2_GPU_POOL_GRANULE - 1) & ~(OP2_GPU_POOL_GRANULE - 1);
		if (op2_pool_alloc(len, &pool_off)) {
			desc = IOSubMemoryDescriptor::withSubRange(gPool, (IOByteCount)pool_off, (IOByteCount)len,
			                                            kIODirectionInOut);
			if (desc) {
				pool_len = len;
				kaddr = gPoolKaddr + pool_off;
				phys = gPoolPhys + pool_off;
			} else {
				op2_pool_free(pool_off, len);
			}
		}
	}
	if (!desc) {
		/* G5b: below 4 GB (the identity map's GPU addresses are 32-bit), physically contiguous */
		IOBufferMemoryDescriptor *bmd = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
			kernel_task,
			kIODirectionInOut | kIOMemoryKernelUserShared | kIOMemoryPhysicallyContiguous,
			(vm_size_t)(aligned_size + OP2_GPU_GUARD),
			0x00000000FFFFF000ULL);
		if (bmd) {
			desc = bmd;
			kaddr = bmd->getBytesNoCopy();
			phys = (uint64_t)bmd->getPhysicalAddress();
		}
	}
	if (!desc) {
		IOLockUnlock(fLock);
		if (refusals++ < 20)
		{
			uint64_t pfree = 0, plargest = 0;
			if (gPoolLock) {
				IOLockLock(gPoolLock);
				for (unsigned i = 0; i < gPoolNFree; i++) {
					pfree += gPoolFree[i].len;
					if (gPoolFree[i].len > plargest)
						plargest = gPoolFree[i].len;
				}
				IOLockUnlock(gPoolLock);
			}
			IOLog("OP2 GPU Client: gemNew %llu bytes refused: no contiguous memory below 4 GB (held %llu bytes; "
			      "pool %llu MB, free %llu KB in %u ranges, largest %llu KB)\n", req->size, gHeldBytes,
			      gPoolBytes >> 20, pfree >> 10, gPoolNFree, plargest >> 10);
		}
		return kIOReturnNoMemory;
	}

	bzero(kaddr, (size_t)aligned_size);
	flushDcache((vm_offset_t)kaddr, (vm_size_t)aligned_size);
	op2_fill((uint32_t *)kaddr + aligned_size / 4, OP2_GPU_GUARD, kGuardWord);

	fBuffers[slot].desc = desc;
	fBuffers[slot].kernel_addr = kaddr;
	fBuffers[slot].phys_addr = phys;
	fBuffers[slot].size = aligned_size;
	fBuffers[slot].alloc_size = aligned_size + OP2_GPU_GUARD;
	fBuffers[slot].owned = true;
	fBuffers[slot].pool_off = pool_off;
	fBuffers[slot].pool_len = pool_len;
	fBuffers[slot].flags = req->flags;
	fBuffers[slot].fence = 0;
	fBuffers[slot].in_use = true;
	fBufferCount++;
	if (fOwner) fOwner->mapRange(phys, aligned_size);

	req->handle = (uint32_t)(slot + 1);
	static uint32_t sGemNewLogged = 0;
	if (sGemNewLogged < 4 || aligned_size >= (4ULL << 20)) {   /* first few, and big ones */
		sGemNewLogged++;
		IOLog("OP2 GPU Client: gemNew handle %u (size %llu bytes, phys 0x%llx%s)\n",
		      req->handle, aligned_size, phys, pool_len ? ", pool" : "");
	}

	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn
OP2GPUUserClient::gemInfo(struct msm_gem_info *req)
{
	IOLockLock(fLock);
	struct op2_gpu_client_buffer *b = getBuffer(req->handle);
	if (!b) {
		IOLockUnlock(fLock);
		return kIOReturnNotFound;
	}

	switch (req->info) {
	case MSM_INFO_GET_OFFSET:
	case MSM_INFO_GET_IOVA:
		req->value = b->phys_addr;
		break;
	default:
		req->value = 0;
		break;
	}

	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn
OP2GPUUserClient::gemCpuPrep(struct msm_gem_cpu_prep *req)
{
	IOLockLock(fLock);
	struct op2_gpu_client_buffer *b = getBuffer(req->handle);
	if (!b) {
		IOLockUnlock(fLock);
		return kIOReturnNotFound;
	}
	uint32_t fence = b->fence;
	IOLockUnlock(fLock);

	if (fence != 0 && fOwner) {
		uint32_t timeout_ms = op2_gpu_deadline_ms(&req->timeout);
		bool ok = fOwner->waitForFence(fence, timeout_ms);
		describeFaults("cpu_prep");
		if (!ok) {
			return kIOReturnTimeout;
		}
	}
	return kIOReturnSuccess;
}

/*
 * Name each new SMMU fault (IOVA = physical) against this client's buffers: inside one (its guard band),
 * how far past the end of the nearest buffer below, how far before the nearest above. Caller must not hold fLock.
 */
void
OP2GPUUserClient::describeFaults(const char *where)
{
	if (!fOwner || !fOwner->fXlate) return;
	fOwner->pollSMMUFault();
	uint32_t total = fOwner->fFaultCount;
	if (total == fFaultsSeen) return;
	IOLockLock(fLock);
	IOLog("OP2 GPU SMMU: [%s] %u new fault latch(es), total %u (%u MULTI, %u writes), FAR 0x%08x..0x%08x\n",
	      where, total - fFaultsSeen, total, fOwner->fFaultMulti, fOwner->fFaultWrites,
	      fOwner->fFaultMin, fOwner->fFaultMax);
	for (uint32_t n = fFaultsSeen; n < total && n < OP2GPU::kOP2FaultLog; n++) {
		const OP2GPU::op2_smmu_fault *f = &fOwner->fFaults[n];
		uint64_t far = f->far;
		int below = -1, above = -1;
		for (uint32_t i = 0; i < OP2_GPU_MAX_BUFFERS; i++) {
			struct op2_gpu_client_buffer *b = &fBuffers[i];
			if (!b->in_use || !b->size) continue;
			if (b->phys_addr <= far && (below < 0 || b->phys_addr > fBuffers[below].phys_addr)) below = (int)i;
			if (b->phys_addr > far && (above < 0 || b->phys_addr < fBuffers[above].phys_addr)) above = (int)i;
		}
		char where_buf[160];
		if (below >= 0 && far < fBuffers[below].phys_addr + fBuffers[below].size) {
			snprintf(where_buf, sizeof(where_buf), "INSIDE buffer %d (+0x%llx of 0x%llx): mapped page faulted?!",
			         below + 1, far - fBuffers[below].phys_addr, fBuffers[below].size);
		} else if (below >= 0) {
			struct op2_gpu_client_buffer *b = &fBuffers[below];
			snprintf(where_buf, sizeof(where_buf), "0x%llx past the end of buffer %d (phys 0x%llx, 0x%llx bytes%s)",
			         far - (b->phys_addr + b->size), below + 1, b->phys_addr, b->size,
			         far < b->phys_addr + b->alloc_size ? ", in its guard band" : "");
		} else {
			snprintf(where_buf, sizeof(where_buf), "below every buffer of this client");
		}
		IOLog("OP2 GPU SMMU FAULT #%u: %s 0x%08x FSR=0x%08x FSYNR0=0x%08x (fence %u retired): %s%s\n",
		      n + 1, (f->fsynr0 & (1U << 4)) ? "WRITE" : "read", f->far, f->fsr, f->fsynr0, f->fence, where_buf,
		      above >= 0 ? "" : "; above every buffer");
		if (above >= 0) {
			IOLog("OP2 GPU SMMU FAULT #%u:   0x%llx before buffer %d (phys 0x%llx, 0x%llx bytes)\n",
			      n + 1, fBuffers[above].phys_addr - far, above + 1, fBuffers[above].phys_addr, fBuffers[above].size);
		}
	}
	if (total > OP2GPU::kOP2FaultLog && fFaultsSeen < OP2GPU::kOP2FaultLog) {
		IOLog("OP2 GPU SMMU: only the first %u latches are kept; the rest show in the count and FAR range\n",
		      (uint32_t)OP2GPU::kOP2FaultLog);
	}
	fFaultsSeen = total;
	IOLockUnlock(fLock);
}

IOReturn
OP2GPUUserClient::gemCpuFini(uint32_t handle)
{
	IOLockLock(fLock);
	struct op2_gpu_client_buffer *b = getBuffer(handle);
	if (!b) {
		IOLockUnlock(fLock);
		return kIOReturnNotFound;
	}
	if (b->kernel_addr && b->size > 0) {
		flushDcache((vm_offset_t)b->kernel_addr, (vm_size_t)b->size);
	}
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn
OP2GPUUserClient::gemSubmit(struct msm_gem_submit *req)
{
	if (!fOwner || !fOwner->isHardwareReady()) {
		return kIOReturnNotReady;
	}
	if (req->nr_cmds == 0 || req->nr_cmds > 128 || req->nr_bos == 0 || req->nr_bos > 256) {
		return kIOReturnBadArgument;
	}

	struct msm_gem_submit_bo *bos = (struct msm_gem_submit_bo *)IOMallocData(req->nr_bos * sizeof(struct msm_gem_submit_bo));
	struct msm_gem_submit_cmd *cmds = (struct msm_gem_submit_cmd *)IOMallocData(req->nr_cmds * sizeof(struct msm_gem_submit_cmd));
	if (!bos || !cmds) {
		if (bos) IOFreeData(bos, req->nr_bos * sizeof(struct msm_gem_submit_bo));
		if (cmds) IOFreeData(cmds, req->nr_cmds * sizeof(struct msm_gem_submit_cmd));
		return kIOReturnNoMemory;
	}

	if (copyin((user_addr_t)req->bos, bos, req->nr_bos * sizeof(*bos)) != 0 ||
	    copyin((user_addr_t)req->cmds, cmds, req->nr_cmds * sizeof(*cmds)) != 0) {
		IOFreeData(bos, req->nr_bos * sizeof(struct msm_gem_submit_bo));
		IOFreeData(cmds, req->nr_cmds * sizeof(struct msm_gem_submit_cmd));
		return kIOReturnVMError;
	}

	IOLockLock(fLock);

	/*
	 * 1. CPU cache lines of the buffers named in the BO table.
	 * This used to clean+invalidate EVERY buffer in full on every submission: the 11 MB screen surface, the 28 MB
	 * wallpaper target, every texture (docs/FABLE-REVIEW.md A4). Buffers made by gemNew (owned) are mapped
	 * write-combined into the client (clientMemoryForType) and were cleaned once when made: they have no dirty
	 * lines, so they are skipped. Surfaces (not owned) are cached memory a CPU may have drawn into: small ones are
	 * still cleaned here; large ones rely on the clean at IOSurface unlock (OP2Surface, metal>=2). gpu_clean=1
	 * restores the old behaviour. The bytes cleaned and skipped are logged every 512 submissions.
	 */
	/* Round fable-18: skipping ALL owned buffers hung the GPU (their userland mappings hold dirty lines). Round
	 * fable-30: cleaning everything cost ~28 MB of dc civac per submission (the panel stalled). Now: an owned buffer
	 * is cleaned only if the client has mapped it (user_mapped, set in clientMemoryForType: a buffer only the GPU
	 * fills was cleaned once by gemNew and holds no CPU lines); a surface is skipped under metal>=2 (a CPU writer's
	 * lines are cleaned at IOSurface unlock, OP2Surface). gpu_clean=1: clean everything, as before. */
	static uint32_t gpu_clean_all = 0, gpu_clean_parsed = 0, gpu_metal = 0;
	static uint64_t sCleaned, sSkipped;
	static uint32_t sSubmits;
	if (!gpu_clean_parsed) {
		gpu_clean_parsed = 1;
		PE_parse_boot_argn("gpu_clean", &gpu_clean_all, sizeof(gpu_clean_all));
		PE_parse_boot_argn("metal", &gpu_metal, sizeof(gpu_metal));
		if (gpu_metal < 2)
			gpu_clean_all = 1;   /* the surface rule needs the unlock clean, which is metal>=2 only */
	}
	for (uint32_t i = 0; i < req->nr_bos; i++) {
		struct op2_gpu_client_buffer *b = getBuffer(bos[i].handle);
		if (b && b->kernel_addr) {
			if (gpu_clean_all || (b->owned && b->user_mapped)) {
				flushDcache((vm_offset_t)b->kernel_addr, (vm_size_t)b->size);
				sCleaned += b->size;
			} else {
				sSkipped += b->size;
			}
		}
	}
	if ((++sSubmits & 511) == 0) {
		IOLog("OP2 GPU: 512 submissions: cache cleaned over %llu KB, skipped %llu KB\n", sCleaned >> 10,
		      sSkipped >> 10);
		sCleaned = sSkipped = 0;
	}

	/* 2. Process each command buffer and submit as indirect buffer */
	uint32_t last_fence = 0;
	IOReturn ret = kIOReturnSuccess;

	for (uint32_t i = 0; i < req->nr_cmds; i++) {
		if (cmds[i].type != MSM_SUBMIT_CMD_BUF) {
			continue;
		}
		if (cmds[i].submit_idx >= req->nr_bos) {
			ret = kIOReturnBadArgument;
			break;
		}
		uint32_t handle = bos[cmds[i].submit_idx].handle;
		struct op2_gpu_client_buffer *b = getBuffer(handle);
		if (!b) {
			ret = kIOReturnNotFound;
			break;
		}
		if ((cmds[i].size & 3) != 0 || (cmds[i].submit_offset & 3) != 0 ||
		    (uint64_t)cmds[i].submit_offset + cmds[i].size > b->size) {
			ret = kIOReturnBadArgument;
			break;
		}

		uint64_t ib_phys = b->phys_addr + cmds[i].submit_offset;
		uint32_t ib_words = cmds[i].size / 4;
		uint32_t fence = 0;

		if (!fOwner->submitIndirectBuffer(ib_phys, ib_words, &fence)) {
			ret = kIOReturnTimeout;
			break;
		}
		b->fence = fence;
		last_fence = fence;
	}
	/* every buffer named in the submission is busy until its last fence (cpuPrep waits on it) */
	if (ret == kIOReturnSuccess && last_fence) {
		for (uint32_t i = 0; i < req->nr_bos; i++) {
			struct op2_gpu_client_buffer *ub = getBuffer(bos[i].handle);
			if (ub) ub->fence = last_fence;
		}
	}

	IOLockUnlock(fLock);

	IOFreeData(bos, req->nr_bos * sizeof(struct msm_gem_submit_bo));
	IOFreeData(cmds, req->nr_cmds * sizeof(struct msm_gem_submit_cmd));

	if (ret == kIOReturnSuccess) {
		req->fence = last_fence;
		static uint32_t sSubmitsLogged = 0;
		if (sSubmitsLogged < 8 || (last_fence % 1000) == 0) {
			sSubmitsLogged++;
			IOLog("OP2 GPU Client: gemSubmit fence %u (%u cmds, %u bos)\n", last_fence, req->nr_cmds, req->nr_bos);
		}
	}
	return ret;
}

IOReturn
OP2GPUUserClient::gemClose(uint32_t handle)
{
	IOLockLock(fLock);
	struct op2_gpu_client_buffer *b = getBuffer(handle);
	if (!b) {
		IOLockUnlock(fLock);
		return kIOReturnNotFound;
	}

	releaseBuffer(b, handle);
	fBufferCount--;

	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn
OP2GPUUserClient::waitFence(struct msm_wait_fence *req)
{
	if (!fOwner) return kIOReturnNotReady;
	uint32_t timeout_ms = op2_gpu_deadline_ms(&req->timeout);

	bool ok = fOwner->waitForFence(req->fence, timeout_ms);
	describeFaults("wait_fence");
	return ok ? kIOReturnSuccess : kIOReturnTimeout;
}

IOReturn
OP2GPUUserClient::clientMemoryForType(UInt32 type,
    IOOptionBits *options,
    IOMemoryDescriptor **memory)
{
	IOLockLock(fLock);
	struct op2_gpu_client_buffer *b = getBuffer(type);
	if (!b || !b->desc) {
		IOLockUnlock(fLock);
		return kIOReturnNotFound;
	}
	b->desc->retain();
	*memory = b->desc;
	b->user_mapped = true;   /* from now on its lines may be dirty: gemSubmit cleans it */
	/* write-combined, as freedreno/hakios expect (no CPU cache maintenance in userland) */
	*options = (b->flags & 1) ? kIOMapInhibitCache : kIOMapWriteCombineCache;
	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn
OP2GPUUserClient::externalMethod(uint32_t selector,
    IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch,
    __unused OSObject *target,
    __unused void *reference)
{
	if (!args) return kIOReturnBadArgument;

	switch (selector) {
	case kMsmGetParam: {
		if (!args->structureInput || args->structureInputSize < sizeof(struct msm_param)) {
			return kIOReturnBadArgument;
		}
		struct msm_param param;
		memcpy(&param, args->structureInput, sizeof(param));

		switch (param.param) {
		case MSM_PARAM_GPU_ID:
			/* the model number, as Linux msm answers it: 430. Mesa looked up 0x4030002
			 * here and refused the GPU ("unsupported GPU id", 2026-11-11 G3) */
			param.value = 430;
			break;
		case MSM_PARAM_CHIP_ID:
			param.value = 0x04030002; /* core 4, major 3, minor 0, patch 2: Adreno 430 v2 */
			break;
		case MSM_PARAM_GMEM_SIZE:
			param.value = 0x180000;   /* 1.5 MB GMEM */
			break;
		case MSM_PARAM_GMEM_BASE:
			param.value = 0x00000000;
			break;
		case MSM_PARAM_MAX_FREQ:
			param.value = 600000000;  /* 600 MHz */
			break;
		case MSM_PARAM_TIMESTAMP:
			param.value = (uint64_t)mach_absolute_time();
			break;
		case 7:  /* PRIORITIES */
			param.value = 1;
			break;
		case 8:  /* PP_PGTABLE: no per-process page tables (SMMU in bypass) */
			param.value = 0;
			break;
		case 9:  /* FAULTS: GPU resets after hangs */
			param.value = fOwner ? fOwner->fHangs : 0;
			break;
		case 10: /* SUSPENDS */
			param.value = 0;
			break;
		default:
			return kIOReturnBadArgument;
		}
		return op2_gpu_struct_out(args, &param, sizeof(param));
	}

	case kMsmSetParam:
		return kIOReturnSuccess;

	case kMsmSubmitQueueNew: {   /* one ring: every queue is queue 0 */
		struct { uint32_t flags, prio, id; } q = {0, 0, 0};
		if (args->structureInput && args->structureInputSize >= sizeof(q)) {
			memcpy(&q, args->structureInput, sizeof(q));
		}
		q.id = 0;
		return op2_gpu_struct_out(args, &q, sizeof(q));
	}
	case kMsmSubmitQueueClose:
		return kIOReturnSuccess;

	case kMsmSubmitQueueQuery:   /* nothing to report (Linux answers unknown params the same way) */
		return kIOReturnUnsupported;

	case kMsmGemMadvise: {   /* freedreno's BO cache: buffers are never purged here, so always retained */
		struct { uint32_t handle, madv, retained; } m = {0, 0, 0};
		if (!args->structureInput || args->structureInputSize < sizeof(m)) {
			return kIOReturnBadArgument;
		}
		memcpy(&m, args->structureInput, sizeof(m));
		IOLockLock(fLock);
		bool ok = getBuffer(m.handle) != NULL;
		IOLockUnlock(fLock);
		if (!ok) return kIOReturnNotFound;
		m.retained = 1;
		return op2_gpu_struct_out(args, &m, sizeof(m));
	}

	case kMsmGemNew: {
		if (!args->structureInput || args->structureInputSize < sizeof(struct msm_gem_new)) {
			return kIOReturnBadArgument;
		}
		struct msm_gem_new req;
		memcpy(&req, args->structureInput, sizeof(req));
		IOReturn ret = gemNew(&req);
		if (ret == kIOReturnSuccess) {
			return op2_gpu_struct_out(args, &req, sizeof(req));
		}
		return ret;
	}

	case kMsmGemInfo: {
		if (!args->structureInput || args->structureInputSize < sizeof(struct msm_gem_info)) {
			return kIOReturnBadArgument;
		}
		struct msm_gem_info req;
		memcpy(&req, args->structureInput, sizeof(req));
		IOReturn ret = gemInfo(&req);
		if (ret == kIOReturnSuccess) {
			return op2_gpu_struct_out(args, &req, sizeof(req));
		}
		return ret;
	}

	case kMsmGemCpuPrep: {
		if (!args->structureInput || args->structureInputSize < sizeof(struct msm_gem_cpu_prep)) {
			return kIOReturnBadArgument;
		}
		struct msm_gem_cpu_prep req;
		memcpy(&req, args->structureInput, sizeof(req));
		return gemCpuPrep(&req);
	}

	case kMsmGemCpuFini: {
		uint32_t handle = 0;
		if (args->structureInput && args->structureInputSize >= 4) {
			handle = *(const uint32_t *)args->structureInput;
		} else if (args->scalarInputCount >= 1) {
			handle = (uint32_t)args->scalarInput[0];
		} else {
			return kIOReturnBadArgument;
		}
		return gemCpuFini(handle);
	}

	case kMsmGemSubmit: {
		if (!args->structureInput || args->structureInputSize < sizeof(struct msm_gem_submit)) {
			return kIOReturnBadArgument;
		}
		struct msm_gem_submit req;
		memcpy(&req, args->structureInput, sizeof(req));
		IOReturn ret = gemSubmit(&req);
		if (ret == kIOReturnSuccess) {
			return op2_gpu_struct_out(args, &req, sizeof(req));
		}
		return ret;
	}

	case kMsmWaitFence: {
		if (!args->structureInput || args->structureInputSize < sizeof(struct msm_wait_fence)) {
			return kIOReturnBadArgument;
		}
		struct msm_wait_fence req;
		memcpy(&req, args->structureInput, sizeof(req));
		return waitFence(&req);
	}

	case kDrmGemClose: {
		uint32_t handle = 0;
		if (args->structureInput && args->structureInputSize >= 4) {
			handle = *(const uint32_t *)args->structureInput;
		} else if (args->scalarInputCount >= 1) {
			handle = (uint32_t)args->scalarInput[0];
		} else {
			return kIOReturnBadArgument;
		}
		return gemClose(handle);
	}

	case kOP2Surface: {
		if (!args->structureInput || args->structureInputSize < sizeof(struct op2_surface_req)) {
			return kIOReturnBadArgument;
		}
		struct op2_surface_req req;
		memcpy(&req, args->structureInput, sizeof(req));

		uint32_t sid = req.surface_id ? req.surface_id : 1;
		OP2Surface *s = op2_surface_copy(sid);
		if (!s) {
			IOLog("OP2 GPU Client: kOP2Surface surface %u not found\n", sid);
			return kIOReturnNotFound;
		}

		IOLockLock(fLock);

		int slot = -1;
		for (uint32_t i = 0; i < OP2_GPU_MAX_BUFFERS; i++) {
			if (!fBuffers[i].in_use) {
				slot = (int)i;
				break;
			}
		}
		if (slot == -1) {
			IOLockUnlock(fLock);
			s->release();
			return kIOReturnNoSpace;
		}

		IOPhysicalLength segLen = 0;
		uint64_t phys = 0;
		if (s->memory) {
			phys = (uint64_t)s->memory->getPhysicalSegment(0, &segLen);
		}

		if (phys == 0 && sid == 1) {
			phys = 0x03400000ULL;
		}

		uint64_t buf_size = s->allocSize ? s->allocSize : ((uint64_t)s->bytesPerRow * s->height);
		if (buf_size == 0) {
			buf_size = 1088 * 1920 * 4;
		}

		if (s->memory) {
			s->memory->retain();
		}

		fBuffers[slot].desc = s->memory;
		fBuffers[slot].kernel_addr = (void *)s->kernelAddress();
		fBuffers[slot].phys_addr = phys;
		fBuffers[slot].size = buf_size;
		fBuffers[slot].flags = 1; /* Framebuffer uncached mapping */
		fBuffers[slot].owned = false;   /* the surface's memory: no guard band, never held */
		fBuffers[slot].alloc_size = buf_size;
		fBuffers[slot].fence = 0;
		fBuffers[slot].in_use = true;
		fBufferCount++;
		if (fOwner) {
			if (s->memory) fOwner->mapDescriptor(s->memory, buf_size, true);
			else fOwner->mapRange(phys, buf_size);   /* the panel carveout (physically contiguous) */
		}
		if (s->memory && segLen < buf_size) {
			IOLog("OP2 GPU Client: surface %u is NOT physically contiguous (first segment %llu of %llu bytes): "
			      "the GPU sees it at phys 0x%llx as if it were\n", sid, (uint64_t)segLen, buf_size, phys);
		}

		req.handle = (uint32_t)(slot + 1);
		req.phys_addr = phys;
		req.size = buf_size;
		req.width = s->width ? s->width : 1080;
		req.height = s->height ? s->height : 1920;
		req.stride = s->bytesPerRow ? s->bytesPerRow : (1088 * 4);
		req.format = s->pixelFormat;

		IOLog("OP2 GPU Client: kOP2Surface bound surface %u -> handle %u (phys 0x%llx, size %llu, %ux%u stride %u)\n",
		      sid, req.handle, phys, buf_size, req.width, req.height, req.stride);

		IOLockUnlock(fLock);
		s->release();

		return op2_gpu_struct_out(args, &req, sizeof(req));
	}

	case kOP2Log: {   /* a line from a userland GPU tool into the kernel log (ramoops) */
		char line[200];
		uint32_t len = 0;
		if (args->structureInput && args->structureInputSize) {
			len = args->structureInputSize;
			if (len > sizeof(line) - 1) len = sizeof(line) - 1;
			memcpy(line, args->structureInput, len);
		} else if (args->scalarInputCount) {
			/* the line packed into up to 16 scalars (OP2Metal: no struct-method import to spare) */
			uint32_t n = args->scalarInputCount > 16 ? 16 : args->scalarInputCount;
			for (uint32_t i = 0; i < n; i++) {
				uint64_t w = args->scalarInput[i];
				memcpy(line + 8 * i, &w, 8);
			}
			len = (uint32_t)strnlen(line, 8 * n);
		}
		if (len == 0) return kIOReturnBadArgument;
		line[len] = 0;
		IOLog("OP2 GPU user: %s\n", line);
		return kIOReturnSuccess;
	}

	case kOP2Show: {
		op2_fb_kick_panel_external();
		static uint32_t sShowLogged = 0;
		if (++sShowLogged <= 3) {
			IOLog("OP2 GPU Client: kOP2Show kicked display panel (#%u)\n", sShowLogged);
		}
		return kIOReturnSuccess;
	}

	default:
		IOLog("OP2 GPU Client: unhandled selector 0x%x\n", selector);
		return kIOReturnUnsupported;
	}
}

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
