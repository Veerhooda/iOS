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

#include "OP2GPU.h"
#include "OP2Surface.h"
#include "op2_a430_fw.h"
#include "op2_rpm.h"
#include <pexpert/pexpert.h>
#include <libkern/OSByteOrder.h>
#include <sys/systm.h>
#include <mach/mach_time.h>

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

bool
OP2GPU::initAdrenoRegisters(void)
{
	IOLog("OP2 GPU: resetting Adreno 430 core...\n");
	writeGPU(A4XX_RBBM_SW_RESET_CMD, 1);
	IODelay(100);
	writeGPU(A4XX_RBBM_SW_RESET_CMD, 0);
	IODelay(100);

	uint32_t status = readGPU(A4XX_RBBM_STATUS);
	uint32_t pwr = readGPU(A4XX_RBBM_POWER_STATUS);
	IOLog("OP2 GPU: Adreno core status=0x%08x power_status=0x%08x\n", status, pwr);

	/* Basic RBBM Configuration */
	writeGPU(A4XX_RBBM_GPU_BUSY_MASKED, 0xffffffff);
	writeGPU(A4XX_RBBM_WAIT_IDLE_CLOCKS_CTL, 0x10);
	writeGPU(A4XX_RBBM_WAIT_IDLE_CLOCKS_CTL2, 0x30);
	writeGPU(A4XX_RBBM_AHB_CTL0, 0x00000001);
	writeGPU(A4XX_RBBM_AHB_CTL1, 0xa6ffffff);
	writeGPU(A4XX_RBBM_RBBM_CTL, 0x00000030);
	writeGPU(A4XX_RBBM_PERFCTR_CTL, 0x01);

	/* GMEM/OCMEM address and cache configuration */
	writeGPU(A4XX_RB_GMEM_BASE_ADDR, 0);
	writeGPU(A4XX_UCHE_CACHE_WAYS_VFD, 0x07);
	writeGPU(A4XX_UCHE_TRAP_BASE_LO, 0xffff0000);
	writeGPU(A4XX_UCHE_TRAP_BASE_HI, 0xffff0000);

	/* CP Debug default */
	writeGPU(A4XX_CP_DEBUG, (1U << 25));

	return true;
}

bool
OP2GPU::initRingBuffer(void)
{
	IOPhysicalAddress phys = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	fRingBufferVirt = (uint32_t *)IOMallocContiguous(OP2_GPU_RB_SIZE, 4096, &phys);
#pragma clang diagnostic pop
	if (!fRingBufferVirt) {
		IOLog("OP2 GPU: ERROR: failed to allocate %u bytes contiguous ringbuffer\n",
		      OP2_GPU_RB_SIZE);
		return false;
	}

	fRingBufferPhys = (uint64_t)phys;
	fWPTR = 0;
	bzero(fRingBufferVirt, OP2_GPU_RB_SIZE);
	flushDcache((vm_offset_t)fRingBufferVirt, OP2_GPU_RB_SIZE);

	IOLog("OP2 GPU: Ringbuffer allocated: virt=%p phys=0x%llx size=%u bytes (%u dwords)\n",
	      fRingBufferVirt, fRingBufferPhys, OP2_GPU_RB_SIZE, OP2_GPU_RB_DWORDS);

	IOPhysicalAddress status_phys = 0;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	fStatusPageVirt = (uint32_t *)IOMallocContiguous(4096, 4096, &status_phys);
#pragma clang diagnostic pop
	if (fStatusPageVirt) {
		fStatusPagePhys = (uint64_t)status_phys;
		bzero(fStatusPageVirt, 4096);
		flushDcache((vm_offset_t)fStatusPageVirt, 4096);
		IOLog("OP2 GPU: Status/fence page allocated: virt=%p phys=0x%llx\n",
		      fStatusPageVirt, fStatusPagePhys);
	}

	/*
	 * CP_RB_CNTL:
	 * Size in quadwords = 32KB / 8 = 4096 quadwords.
	 * ilog2(4096) = 12.
	 * Bit 27: disable shadow RPTR to use direct register reading.
	 */
	uint32_t rb_cntl = (12 & 0x3f) | (1U << 27);
	writeGPU(A4XX_CP_RB_CNTL, rb_cntl);
	writeGPU(A4XX_CP_RB_BASE, (uint32_t)fRingBufferPhys);
	writeGPU(A4XX_CP_RB_WPTR, 0);
	writeGPU(A4XX_CP_RB_RPTR, 0);

	return true;
}

bool
OP2GPU::waitRPTR(uint32_t target_rptr, uint32_t timeout_us)
{
	for (uint32_t i = 0; i < timeout_us / 10; i++) {
		uint32_t rptr = readGPU(A4XX_CP_RB_RPTR);
		if (rptr >= target_rptr) {
			return true;
		}
		IODelay(10);
	}
	uint32_t final_rptr = readGPU(A4XX_CP_RB_RPTR);
	uint32_t rbbm_stat = readGPU(A4XX_RBBM_STATUS);
	uint32_t int_stat = readGPU(A4XX_RBBM_INT_0_STATUS);
	uint32_t ahb_err = readGPU(A4XX_RBBM_AHB_ERROR_STATUS);
	uint32_t me_stat = readGPU(A4XX_CP_ME_STATUS);
	IOLog("OP2 GPU: waitRPTR timeout! expected=%u actual=%u (RBBM_STATUS=0x%08x INT=0x%08x AHB_ERR=0x%08x ME_STAT=0x%08x)\n",
	      target_rptr, final_rptr, rbbm_stat, int_stat, ahb_err, me_stat);
	return false;
}

bool
OP2GPU::submitPacket(const uint32_t *words, uint32_t count)
{
	if (!fRingBufferVirt) return false;

	if (fWPTR + count >= OP2_GPU_RB_DWORDS) {
		/* Ringbuffer full/wrap: wait for GPU to finish current queue */
		if (!waitRPTR(fWPTR, 50000)) {
			IOLog("OP2 GPU: ringbuffer rewind wait timed out!\n");
			return false;
		}
		fWPTR = 0;
		writeGPU(A4XX_CP_RB_WPTR, 0);
		writeGPU(A4XX_CP_RB_RPTR, 0);
	}

	for (uint32_t i = 0; i < count; i++) {
		fRingBufferVirt[fWPTR + i] = words[i];
	}

	/* Clean cache to Point of Coherency (DRAM) so GPU CP sees it over AXI DMA */
	flushDcache((vm_offset_t)&fRingBufferVirt[fWPTR], count * sizeof(uint32_t));

	fWPTR += count;

	OSMemoryBarrier();
	writeGPU(A4XX_CP_RB_WPTR, fWPTR);
	return true;
}

bool
OP2GPU::bootstrapMicrocode(void)
{
	IOLog("OP2 GPU: loading Adreno 430 microcode via AHB MMIO (PFP=%u words, PM4=%u words)...\n",
	      OP2_A430_PFP_FW_WORDS - 1, OP2_A430_PM4_FW_WORDS - 1);

	/* Halt micro-engine while loading instruction RAM */
	writeGPU(A4XX_CP_ME_CNTL, 1);

	/*
	 * Direct AHB Microcode Load (Freedreno / Linux KGSL fallback):
	 * Writes the microcode instructions directly into CP ME RAM and PFP RAM.
	 *
	 * pm4_fw[1..2388] -> A4XX_CP_ME_RAM_DATA at addr 0
	 * pfp_fw[1..1072] -> A4XX_CP_PFP_UCODE_DATA at addr 0
	 */
	writeGPU(A4XX_CP_ME_RAM_WADDR, 0);
	for (uint32_t i = 1; i < OP2_A430_PM4_FW_WORDS; i++) {
		writeGPU(A4XX_CP_ME_RAM_DATA, op2_a430_pm4_fw[i]);
	}

	writeGPU(A4XX_CP_PFP_UCODE_ADDR, 0);
	for (uint32_t i = 1; i < OP2_A430_PFP_FW_WORDS; i++) {
		writeGPU(A4XX_CP_PFP_UCODE_DATA, op2_a430_pfp_fw[i]);
	}

	/* Clear ME_HALT to start micro-engine */
	writeGPU(A4XX_CP_ME_CNTL, 0);
	IODelay(100);

	uint32_t me_cntl = readGPU(A4XX_CP_ME_CNTL);
	uint32_t rbbm_stat = readGPU(A4XX_RBBM_STATUS);
	IOLog("OP2 GPU: micro-engine unhalted (CP_ME_CNTL=0x%08x, RBBM_STATUS=0x%08x)\n",
	      me_cntl, rbbm_stat);

	return true;
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
	fEnabled = false;
	fHardwareInitialized = false;
	fLock = IOLockAlloc();

	return true;
}

bool
OP2GPU::start(IOService *provider)
{
	if (!IOService::start(provider))
		return false;

	uint32_t gpu_arg = 1;
	PE_parse_boot_argn("gpu", &gpu_arg, sizeof(gpu_arg));
	if (gpu_arg == 0) {
		IOLog("OP2 GPU: driver disabled (pass gpu=1 in boot-args to enable)\n");
		registerService();
		return true;
	}

	uint32_t gpu_hw = 1;
	PE_parse_boot_argn("gpu_hw", &gpu_hw, sizeof(gpu_hw));

	IOLog("OP2 GPU: Initializing Adreno 430 GPU driver (gpu=%u, gpu_hw=%u)...\n", gpu_arg, gpu_hw);

	/* Set device properties in IORegistry */
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
		IOLog("OP2 GPU: Running in safe software-shim mode (gpu_hw=0); direct silicon MMIO bypassed to prevent AXI bus stall\n");
		setProperty("HardwareAccelerated", kOSBooleanFalse);
		registerService();
		IOLog("OP2 GPU: driver published in IORegistry successfully!\n");
		return true;
	}

	/* Direct Hardware Bring-Up (only if gpu_hw=1 is explicitly passed) */
	IOLog("OP2 GPU: Attempting physical silicon bring-up (gpu_hw=1)...\n");

	/* 1. Map MMSS CC and GPU MMIO */
	fMMSSBase = ml_io_map(OP2_MMSSCC_PHYS_BASE, OP2_MMSSCC_PHYS_SIZE);
	if (!fMMSSBase) {
		IOLog("OP2 GPU: ERROR: failed to map MMSS CC at 0x%llx\n", OP2_MMSSCC_PHYS_BASE);
		registerService();
		return true;
	}
	fGPUBase = ml_io_map(OP2_ADRENO_PHYS_BASE, OP2_ADRENO_PHYS_SIZE);
	if (!fGPUBase) {
		IOLog("OP2 GPU: ERROR: failed to map Adreno MMIO at 0x%llx\n", OP2_ADRENO_PHYS_BASE);
		registerService();
		return true;
	}
	IOLog("OP2 GPU: MMIO mapped: MMSSCC=%p Adreno=%p\n", (void *)fMMSSBase, (void *)fGPUBase);

	/* 2. Power on GDSC and branch clocks */
	if (!powerOnClocks()) {
		IOLog("OP2 GPU: powerOnClocks failed (rails/clocks not active); skipping Adreno MMIO to prevent AXI bus hang\n");
		setProperty("HardwareAccelerated", kOSBooleanFalse);
		registerService();
		IOLog("OP2 GPU: driver registered in safe fallback mode\n");
		return true;
	}

	/* 3. Allocate ringbuffer */
	if (!initRingBuffer()) {
		registerService();
		return true;
	}

	/* 4. Configure Adreno core */
	initAdrenoRegisters();

	/* 5. Bootstrap microcode */
	if (!bootstrapMicrocode()) {
		IOLog("OP2 GPU: Microcode bootstrap failed; aborting pipeline start\n");
		registerService();
		return true;
	}

	/* 6. Issue ME Initialization packet */
	uint32_t me_init_cmds[] = {
		cp_type3_packet(CP_ME_INIT, 17),
		0x000003f7,
		0x00000000,
		0x00000000,
		0x00000000,
		0x00000080,
		0x00000100,
		0x00000180,
		0x00006600,
		0x00000150,
		0x0000014e,
		0x00000154,
		0x00000001,
		0x00000000,
		0x00000000,
		0x20000000,
		0x00000000,
		0x00000000
	};
	submitPacket(me_init_cmds, sizeof(me_init_cmds) / sizeof(uint32_t));
	if (waitRPTR(fWPTR, 50000)) {
		IOLog("OP2 GPU: CP_ME_INIT accepted! RPTR=%u WPTR=%u\n",
		      readGPU(A4XX_CP_RB_RPTR), readGPU(A4XX_CP_RB_WPTR));
	} else {
		IOLog("OP2 GPU: WARNING: CP_ME_INIT wait timed out\n");
	}

	/* 7. Issue first CP_NOP packet to test command execution pipeline */
	uint32_t nop_cmds[] = {
		cp_type3_packet(CP_NOP, 4),
		0xdeada430,
		0x12345678,
		0x87654321,
		0xcafe0001
	};
	submitPacket(nop_cmds, sizeof(nop_cmds) / sizeof(uint32_t));
	if (waitRPTR(fWPTR, 50000)) {
		IOLog("OP2 GPU: SUCCESS: Hardware CP_NOP packet executed! RPTR=%u WPTR=%u\n",
		      readGPU(A4XX_CP_RB_RPTR), readGPU(A4XX_CP_RB_WPTR));
		fHardwareInitialized = true;
	} else {
		IOLog("OP2 GPU: WARNING: Hardware CP_NOP packet wait timed out\n");
	}

	setProperty("HardwareAccelerated", fHardwareInitialized ? kOSBooleanTrue : kOSBooleanFalse);
	registerService();
	IOLog("OP2 GPU: driver published in IORegistry successfully!\n");
	return true;
}

void
OP2GPU::stop(IOService *provider)
{
	if (gOP2GPUInstance == this) {
		gOP2GPUInstance = NULL;
	}
	if (fRingBufferVirt) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		IOFreeContiguous(fRingBufferVirt, OP2_GPU_RB_SIZE);
#pragma clang diagnostic pop
		fRingBufferVirt = NULL;
	}
	if (fStatusPageVirt) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
		IOFreeContiguous(fStatusPageVirt, 4096);
#pragma clang diagnostic pop
		fStatusPageVirt = NULL;
	}
	if (fLock) {
		IOLockFree(fLock);
		fLock = NULL;
	}
	IOService::stop(provider);
}

bool
OP2GPU::submitIndirectBuffer(uint64_t phys_addr, uint32_t words, uint32_t *out_fence)
{
	if (!fRingBufferVirt) return false;

	IOLockLock(fLock);

	uint32_t fence = ++fFenceSeq;
	if (fence == 0) fence = ++fFenceSeq;

	uint32_t ib_cmds[8];
	ib_cmds[0] = cp_type3_packet(CP_INDIRECT_BUFFER_PFE, 2);
	ib_cmds[1] = (uint32_t)phys_addr;
	ib_cmds[2] = words;
	ib_cmds[3] = cp_type3_packet(CP_WAIT_FOR_IDLE, 1);
	ib_cmds[4] = 0x00000000;
	ib_cmds[5] = cp_type3_packet(CP_MEM_WRITE, 2);
	ib_cmds[6] = (uint32_t)fStatusPagePhys;
	ib_cmds[7] = fence;

	bool ok = submitPacket(ib_cmds, 8);
	if (ok && out_fence) {
		*out_fence = fence;
	}

	IOLockUnlock(fLock);
	return ok;
}

bool
OP2GPU::isFenceDone(uint32_t fence)
{
	if (fence == 0) return true;
	if (!fStatusPageVirt) return true;

	flushDcache((vm_offset_t)fStatusPageVirt, 64);
	if (fStatusPageVirt[0] >= fence) {
		return true;
	}
	uint32_t rptr = readGPU(A4XX_CP_RB_RPTR);
	uint32_t wptr = readGPU(A4XX_CP_RB_WPTR);
	if (rptr == wptr && rptr > 0) {
		return true;
	}
	return false;
}

bool
OP2GPU::waitForFence(uint32_t fence, uint32_t timeout_ms)
{
	uint32_t loops = (timeout_ms * 1000) / 50;
	for (uint32_t i = 0; i < loops; i++) {
		if (isFenceDone(fence)) {
			return true;
		}
		IODelay(50);
	}
	IOLog("OP2 GPU: waitForFence timeout! fence=%u (status_fence=%u RPTR=%u WPTR=%u)\n",
	      fence, fStatusPageVirt ? fStatusPageVirt[0] : 0,
	      readGPU(A4XX_CP_RB_RPTR), readGPU(A4XX_CP_RB_WPTR));
	return false;
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
	fLock = IOLockAlloc();
	bzero(fBuffers, sizeof(fBuffers));
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
	IOLockLock(fLock);
	for (uint32_t i = 0; i < OP2_GPU_MAX_BUFFERS; i++) {
		if (fBuffers[i].in_use) {
			if (fBuffers[i].fence != 0 && fOwner) {
				fOwner->waitForFence(fBuffers[i].fence, 1000);
			}
			if (fBuffers[i].desc) {
				fBuffers[i].desc->release();
				fBuffers[i].desc = NULL;
			}
			fBuffers[i].in_use = false;
		}
	}
	fBufferCount = 0;
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
	if (slot == -1) {
		IOLockUnlock(fLock);
		return kIOReturnNoSpace;
	}

	IOBufferMemoryDescriptor *desc = IOBufferMemoryDescriptor::inTaskWithOptions(
		kernel_task,
		kIODirectionInOut | kIOMemoryKernelUserShared | kIOMemoryPhysicallyContiguous,
		(vm_size_t)aligned_size,
		4096);
	if (!desc) {
		IOLockUnlock(fLock);
		return kIOReturnNoMemory;
	}

	void *kaddr = desc->getBytesNoCopy();
	uint64_t phys = (uint64_t)desc->getPhysicalAddress();

	bzero(kaddr, (size_t)aligned_size);
	flushDcache((vm_offset_t)kaddr, (vm_size_t)aligned_size);

	fBuffers[slot].desc = desc;
	fBuffers[slot].kernel_addr = kaddr;
	fBuffers[slot].phys_addr = phys;
	fBuffers[slot].size = aligned_size;
	fBuffers[slot].flags = req->flags;
	fBuffers[slot].fence = 0;
	fBuffers[slot].in_use = true;
	fBufferCount++;

	req->handle = (uint32_t)(slot + 1);
	IOLog("OP2 GPU Client: gemNew handle %u (size %llu bytes, phys 0x%llx)\n",
	      req->handle, aligned_size, phys);

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
		uint32_t timeout_ms = (uint32_t)(req->timeout.seconds * 1000 + req->timeout.nanoseconds / 1000000);
		if (timeout_ms == 0) timeout_ms = 1000;
		if (!fOwner->waitForFence(fence, timeout_ms)) {
			return kIOReturnTimeout;
		}
	}
	return kIOReturnSuccess;
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

	/* 1. Flush CPU cache lines for all buffers named in the BO table */
	for (uint32_t i = 0; i < req->nr_bos; i++) {
		struct op2_gpu_client_buffer *b = getBuffer(bos[i].handle);
		if (b && b->kernel_addr) {
			flushDcache((vm_offset_t)b->kernel_addr, (vm_size_t)b->size);
		}
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

	IOLockUnlock(fLock);

	IOFreeData(bos, req->nr_bos * sizeof(struct msm_gem_submit_bo));
	IOFreeData(cmds, req->nr_cmds * sizeof(struct msm_gem_submit_cmd));

	if (ret == kIOReturnSuccess) {
		req->fence = last_fence;
		IOLog("OP2 GPU Client: gemSubmit successful! Issued hardware fence: %u\n", last_fence);
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

	if (b->fence != 0 && fOwner) {
		fOwner->waitForFence(b->fence, 1000);
	}

	if (b->desc) {
		b->desc->release();
		b->desc = NULL;
	}
	b->kernel_addr = NULL;
	b->phys_addr = 0;
	b->size = 0;
	b->fence = 0;
	b->in_use = false;
	fBufferCount--;

	IOLockUnlock(fLock);
	return kIOReturnSuccess;
}

IOReturn
OP2GPUUserClient::waitFence(struct msm_wait_fence *req)
{
	if (!fOwner) return kIOReturnNotReady;
	uint32_t timeout_ms = (uint32_t)(req->timeout.seconds * 1000 + req->timeout.nanoseconds / 1000000);
	if (timeout_ms == 0) timeout_ms = 5000;

	if (fOwner->waitForFence(req->fence, timeout_ms)) {
		IOLog("OP2 GPU Client: waitFence %u verified completed on hardware!\n", req->fence);
		return kIOReturnSuccess;
	}
	return kIOReturnTimeout;
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
	*options = (b->flags & 1) ? kIOMapInhibitCache : 0;
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
		case MSM_PARAM_CHIP_ID:
			param.value = 0x04030002; /* Adreno 430 v2.1 */
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
		default:
			return kIOReturnBadArgument;
		}
		return op2_gpu_struct_out(args, &param, sizeof(param));
	}

	case kMsmSetParam:
		return kIOReturnSuccess;

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
		fBuffers[slot].fence = 0;
		fBuffers[slot].in_use = true;
		fBufferCount++;

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

	case kOP2Show: {
		op2_fb_kick_panel_external();
		IOLog("OP2 GPU Client: kOP2Show kicked display panel\n");
		return kIOReturnSuccess;
	}

	default:
		IOLog("OP2 GPU Client: unhandled selector 0x%x\n", selector);
		return kIOReturnUnsupported;
	}
}

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
