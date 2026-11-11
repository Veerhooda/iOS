/*
 * Built-in IOMobileFramebuffer for OnePlus 2 (MSM8994):
 * The continuous-splash framebuffer (1080x1920 BGRA at 0x03401000 in RAM,
 * scanned out by MDP5 hardware) as the main display, driven by CoreAnimation's
 * CPU renderer.
 *
 * IOMobileFramebuffer.framework counts /arm-io children with
 * device_type "display-subsystem", waits for services matching
 * IOServiceMatching("IOMobileFramebuffer"), opens each with type 0 and
 * reads canvas-width/canvas-height from the service's provider.
 * AppleCLCD matches the /arm-io/disp0 nub.
 *
 * User client selectors used by CoreAnimation:
 *   3  GetLayerDefaultSurface [layer, ?] -> [IOSurface port name]
 *   4  SwapBegin -> [swap id]
 *   5  SwapEnd, struct 0x46c: +0x50 swap id, +0x54 IOSurface id per layer
 *      (4 layers), +0x64 rect a / +0xc4 rect b per layer (int x,y,w,h),
 *      +0x104 enabled layer mask
 *   6  SwapWait, 7 GetID, 8 GetDisplaySize -> [w, h],
 *   12 RequestPowerChange [state]: 0 = display off, else on
 *   18 IsMainDisplay, 28 GetDotPitch,
 *   72 Enable/DisableNotifications [func, refcon, type, 0]
 *
 * SwapEnd records the swap; vsync timer presents frames into panel memory
 * at a throttled rate (boot-arg op2fb_hz, default 15 Hz) to prevent single-core
 * CPU starvation.
 */
#include "OP2Surface.h"
#include "OP2GPU.h"

#if defined(ARM64_BOARD_CONFIG_MSM8994)
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/system.h>
#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <sys/proc.h>
#include "apple_logo.h"

#ifndef IOMallocData
#define IOMallocData(sz) IOMalloc(sz)
#endif
#ifndef IOFreeData
#define IOFreeData(p, sz) IOFree((p), (sz))
#endif

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

static uint64_t op2_fb_last_kick = 0;
static uint64_t op2_fb_kick_count = 0;
static uint64_t op2_fb_present_count; /* moved up: vsyncFired timeline reads it */
class AppleCLCD; /* fwd: full type below, before use */
static AppleCLCD *op2_fb_instance = NULL; /* set in start(), for deadline dump */
static uint32_t op2_fb_last_shown = 0;    /* mirror of present()'s sLastShown */
static uint64_t op2_fb_last_direct_phys = 0; /* physical address of presented surface */
static uint8_t op2_fb_last_rec[0x278];    /* last swap record, for deadline dump */
static uint32_t op2_fb_last_nz = 0;       /* its sampled content */
static uint64_t op2_fb_last_pc = 0;

static void
op2_fb_kick_panel(volatile uint32_t *ctl)
{
	if (ctl == NULL) {
		return;
	}
	uint64_t now = mach_absolute_time();
	/* Spacing to ensure previous DSI transfer finished (60 Hz = ~16.6 ms = 320,000 ticks) */
	if (op2_fb_last_kick != 0 && (now - op2_fb_last_kick) < 280000ULL) {
		return;
	}
	op2_fb_last_kick = now;
	op2_fb_kick_count++;

	__asm__ volatile("dsb sy" ::: "memory");
	ctl[0x18 / 4] = 0x00020041;              /* CTL0_FLUSH: VIG0 (bit 0) | LM0 (bit 6) | CTL (bit 17) */
	__asm__ volatile("dsb sy" ::: "memory");
	ctl[0x1c / 4] = 1;                       /* CTL0_START: kick DSI command-mode transfer */
	__asm__ volatile("dsb sy" ::: "memory");
}

enum {
	kOP2FBGetLayerDefaultSurface  = 3,
	kOP2FBSwapBegin               = 4,
	kOP2FBSwapEnd                 = 5,
	kOP2FBSwapWait                = 6,
	kOP2FBGetID                   = 7,
	kOP2FBGetDisplaySize          = 8,
	kOP2FBRequestPowerChange      = 12,
	kOP2FBIsMainDisplay           = 18,
	kOP2FBGetDotPitch             = 28,
	kOP2FBSurfaceIsReplaceable    = 49,
	kOP2FBNotifications           = 72,
	kOP2FBChangeFrameInfo         = 73,
	kOP2FBSupportedFrameInfo      = 74,
};

#define OP2_FB_SWAP_REC_SIZE          0x46c
#define OP2_FB_LAYERS                 4
#define OP2_FB_NOTIFY_TYPES           8
#define OP2_FB_NOTIFY_VSYNC           5
#define OP2_FB_VSYNC_US               16667   /* 60 Hz */
#define OP2_FB_DOT_PITCH              401     /* ppi, 5.5" 1080x1920 */
#define OP2_FB_FORMAT_BGRA            0x42475241
#define OP2_FB_DEFAULT_PHYS           0x03401000ULL
#define OP2_FB_DEFAULT_WIDTH          1080
#define OP2_FB_DEFAULT_HEIGHT         1920
#define OP2_FB_DEFAULT_ROWBYTES       4352    /* 1088 * 4 */

class IOAcceleratorES : public IOService {
	OSDeclareDefaultStructors(IOAcceleratorES);
};
OSDefineMetaClassAndStructors(IOAcceleratorES, IOService);

class AGXAccelerator : public IOAcceleratorES {
	OSDeclareDefaultStructors(AGXAccelerator);
public:
	virtual IOReturn newUserClient(task_t owningTask, void *securityID,
	    UInt32 type, OSDictionary *properties,
	    IOUserClient **handler) APPLE_KEXT_OVERRIDE;
	virtual IOReturn newUserClient(task_t owningTask, void *securityID,
	    UInt32 type, IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(AGXAccelerator, IOAcceleratorES);

IOReturn
AGXAccelerator::newUserClient(task_t owningTask, void *securityID,
    UInt32 type, OSDictionary *properties,
    IOUserClient **handler)
{
	OP2GPU *gpu = OP2GPU::getInstance();
	if (gpu) {
		return gpu->newUserClient(owningTask, securityID, type, properties, handler);
	}
	return kIOReturnNotReady;
}

IOReturn
AGXAccelerator::newUserClient(task_t owningTask, void *securityID,
    UInt32 type, IOUserClient **handler)
{
	return newUserClient(owningTask, securityID, type, NULL, handler);
}

class IOMobileFramebuffer : public IOService {
	OSDeclareDefaultStructors(IOMobileFramebuffer);
};

OSDefineMetaClassAndStructors(IOMobileFramebuffer, IOService);

class IOMobileFramebufferUserClient;

class AppleCLCD : public IOMobileFramebuffer {
	OSDeclareDefaultStructors(AppleCLCD);

	IOWorkLoop         *fWorkLoop;
	IOTimerEventSource *fVsync;
	uint64_t            fVsyncCount;

	static void vsyncFired(OSObject *owner, IOTimerEventSource *sender);

public:
	uint64_t      fPhys;
	uint32_t      fWidth, fHeight, fRowBytes;
	uintptr_t     fPixels;          /* kernel mapping of the panel memory */
	IOMemoryMap  *fMap;
	OP2Surface   *fSurface;         /* the panel memory as an IOSurface */
	IOLock       *fLock;            /* swaps and client list */
	OSArray      *fClients;
	uint32_t      fSwapID;
	bool          fConsolePaused;
	uint8_t       fPending[OP2_FB_SWAP_REC_SIZE];     /* latest swap, not yet shown */
	bool          fDirty;
	uint32_t      fFlushTicks;      /* vsync ticks between panel copies */
	bool          fPowerOff;        /* RequestPowerChange(0) */
	uint32_t      fFadeStep;        /* fade-out steps done while off */
	volatile uint32_t *fMdpCtl;
	volatile uint32_t *fMdpPP;
	volatile uint32_t *fDsi0;
	volatile uint32_t *fActivePipe;
	uint32_t           fPipeFlushBit;

	uint64_t      fBootStartMachTime;
	uint32_t      fLastProgressPct;
	bool          fUserlandTookOver;
	bool          fClientUI;             /* op2fb_client_ui: show client surfaces when server is blank */

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	void addClient(IOMobileFramebufferUserClient *client);
	void removeClient(IOMobileFramebufferUserClient *client);
	void swap(const uint8_t *rec);
	void present(const uint8_t *rec);
	void drawProgressBar(uint32_t percent);
};

OSDefineMetaClassAndStructors(AppleCLCD, IOMobileFramebuffer);

class IOMobileFramebufferUserClient : public IOUserClient {
	OSDeclareDefaultStructors(IOMobileFramebufferUserClient);

public:
	AppleCLCD          *fFB;
	task_t              fTask;
	mach_port_t         fPort;
	io_user_reference_t fFunc[OP2_FB_NOTIFY_TYPES];
	io_user_reference_t fRefcon[OP2_FB_NOTIFY_TYPES];

	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE;
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	IOReturn registerNotificationPort(mach_port_t port, UInt32 type,
	    io_user_reference_t refCon) APPLE_KEXT_OVERRIDE;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target,
	    void *reference) APPLE_KEXT_OVERRIDE;
	void notify(uint32_t type, const uint64_t *values, uint32_t count);
};

OSDefineMetaClassAndStructors(IOMobileFramebufferUserClient, IOUserClient);

void
AppleCLCD::drawProgressBar(uint32_t percent)
{
	if (fPixels == 0) return;
	uint8_t *fb = (uint8_t *)fPixels;

	int32_t bar_w = 440;
	int32_t bar_h = 12;
	int32_t bx0 = (int32_t)(fWidth / 2) - (bar_w / 2); /* 540 - 220 = 320 */
	int32_t by0 = 1030;
	int32_t bx1 = bx0 + bar_w;                          /* 760 */
	int32_t by1 = by0 + bar_h;                          /* 1042 */
	if (percent > 100) percent = 100;
	int32_t fill_x = bx0 + (bar_w * (int32_t)percent) / 100;

	int32_t cr = bar_h / 2; /* 6 px radius */
	int32_t cy = by0 + cr;
	int32_t lcx = bx0 + cr;
	int32_t rcx = bx1 - cr;

	for (int32_t y = by0; y < by1; y++) {
		if (y < 0 || y >= (int32_t)fHeight) continue;
		uint32_t *row = (uint32_t *)(void *)(fb + (uint32_t)y * fRowBytes);
		for (int32_t x = bx0; x < bx1; x++) {
			if (x < 0 || x >= (int32_t)fWidth) continue;
			bool inside = true;
			if (x < lcx) {
				int32_t dx = x - lcx;
				int32_t dy = y - cy;
				if (dx * dx + dy * dy > cr * cr) inside = false;
			} else if (x > rcx) {
				int32_t dx = x - rcx;
				int32_t dy = y - cy;
				if (dx * dx + dy * dy > cr * cr) inside = false;
			}
			if (inside) {
				if (x <= fill_x) {
					row[x] = 0xFFFFFFFFu;
				} else {
					row[x] = 0xFF3A3A3Cu;
				}
			}
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	op2_fb_kick_panel(fMdpCtl);
}

extern "C" void
op2_fb_kick_panel_external(void)
{
	if (op2_fb_instance != NULL && op2_fb_instance->fMdpCtl != NULL) {
		__asm__ volatile("dsb sy" ::: "memory");
		op2_fb_kick_panel(op2_fb_instance->fMdpCtl);
	}
}

extern "C" void op2_threaddump_start(void);

bool
AppleCLCD::start(IOService *provider)
{
	PE_Video video;

	if (!IOService::start(provider)) {
		return false;
	}
	op2_threaddump_start(); /* no-op unless op2_deadline is armed */
	PE_current_console(&video);
	fPhys = video.v_baseAddr;
	fWidth = (uint32_t)video.v_width;
	fHeight = (uint32_t)video.v_height;
	fRowBytes = (uint32_t)video.v_rowBytes;
	if (fPhys == 0 || fWidth == 0 || fHeight == 0 || fRowBytes < fWidth * 4) {
		fPhys = OP2_FB_DEFAULT_PHYS;
		fWidth = OP2_FB_DEFAULT_WIDTH;
		fHeight = OP2_FB_DEFAULT_HEIGHT;
		fRowBytes = OP2_FB_DEFAULT_ROWBYTES;
	}

	volatile uint32_t *mmcc = (volatile uint32_t *)ml_io_map(0xfd8c0000, 0x6000);
	fMdpCtl                 = (volatile uint32_t *)ml_io_map(0xfd902000, 0x1000);
	volatile uint32_t *vig0 = (volatile uint32_t *)ml_io_map(0xfd905000, 0x1000);
	volatile uint32_t *rgb0 = (volatile uint32_t *)ml_io_map(0xfd915000, 0x1000);
	volatile uint32_t *dma0 = (volatile uint32_t *)ml_io_map(0xfd925000, 0x1000);
	volatile uint32_t *lm0  = (volatile uint32_t *)ml_io_map(0xfd945000, 0x1000);
	fMdpPP                  = (volatile uint32_t *)ml_io_map(0xfd971000, 0x1000);
	fDsi0                   = (volatile uint32_t *)ml_io_map(0xfd998000, 0x1000);

	if (mmcc != NULL) {
		IOLog("OP2 FB: MMCC MDSS AHB=0x%08x AXI=0x%08x MDP=0x%08x PCLK0=0x%08x BYTE0=0x%08x GDSC=0x%08x\n",
		    mmcc[0x2308 / 4], mmcc[0x2314 / 4], mmcc[0x231c / 4],
		    mmcc[0x2334 / 4], mmcc[0x233c / 4], mmcc[0x2304 / 4]);
	}
	if (fMdpCtl != NULL) {
		IOLog("OP2 FB: CTL0 LAYER0=0x%08x LAYER1=0x%08x TOP=0x%08x FLUSH=0x%08x START=0x%08x\n",
		    fMdpCtl[0x00 / 4], fMdpCtl[0x04 / 4], fMdpCtl[0x14 / 4], fMdpCtl[0x18 / 4], fMdpCtl[0x1c / 4]);
	}
	if (lm0 != NULL) {
		IOLog("OP2 FB: LM0 OP=0x%08x OUT=0x%08x BORDER=0x%08x\n",
		    lm0[0x00 / 4], lm0[0x04 / 4], lm0[0x08 / 4]);
	}
	if (vig0 != NULL) {
		IOLog("OP2 FB: VIG0 SRC0=0x%08x SIZE=0x%08x STRIDE=%u FMT=0x%08x UNPACK=0x%08x\n",
		    vig0[0x14 / 4], vig0[0x00 / 4], vig0[0x24 / 4], vig0[0x30 / 4], vig0[0x34 / 4]);
	}
	if (rgb0 != NULL) {
		IOLog("OP2 FB: RGB0 SRC0=0x%08x SIZE=0x%08x STRIDE=%u FMT=0x%08x UNPACK=0x%08x\n",
		    rgb0[0x14 / 4], rgb0[0x00 / 4], rgb0[0x24 / 4], rgb0[0x30 / 4], rgb0[0x34 / 4]);
	}
	if (dma0 != NULL) {
		IOLog("OP2 FB: DMA0 SRC0=0x%08x SIZE=0x%08x STRIDE=%u FMT=0x%08x UNPACK=0x%08x\n",
		    dma0[0x14 / 4], dma0[0x00 / 4], dma0[0x24 / 4], dma0[0x30 / 4], dma0[0x34 / 4]);
	}
	if (fMdpPP != NULL) {
		IOLog("OP2 FB: PP0 TEAR=0x%08x SYNC_H=0x%08x AUTO=0x%08x LINES=%u\n",
		    fMdpPP[0x00 / 4], fMdpPP[0x08 / 4], fMdpPP[0x38 / 4], fMdpPP[0x28 / 4]);
	}
	if (fDsi0 != NULL) {
		IOLog("OP2 FB: DSI0 CTRL=0x%08x STAT=0x%08x MDP_CTRL=0x%08x TRIG=0x%08x MUX=0x%08x\n",
		    fDsi0[0x00 / 4], fDsi0[0x04 / 4], fDsi0[0x3c / 4], fDsi0[0x80 / 4], fDsi0[0x84 / 4]);
	}

	fActivePipe = NULL;
	fPipeFlushBit = 0;
	const char *pipeName = "NONE";

	if (rgb0 != NULL && rgb0[0x14 / 4] != 0) {
		fActivePipe = rgb0;
		fPipeFlushBit = 1u << 3; /* RGB0 bit 3 */
		pipeName = "RGB0";
	} else if (vig0 != NULL && vig0[0x14 / 4] != 0) {
		fActivePipe = vig0;
		fPipeFlushBit = 1u << 0; /* VIG0 bit 0 */
		pipeName = "VIG0";
	} else if (dma0 != NULL && dma0[0x14 / 4] != 0) {
		fActivePipe = dma0;
		fPipeFlushBit = 1u << 6; /* DMA0 bit 6 */
		pipeName = "DMA0";
	}

	if (fActivePipe != NULL) {
		uint64_t lkPhys = fActivePipe[0x14 / 4];
		uint32_t lkStride = fActivePipe[0x24 / 4];
		uint32_t lkSize = fActivePipe[0x00 / 4];
		IOLog("OP2 FB: active bootloader pipe %s at 0x%llx (size=0x%08x stride=%u)\n",
		    pipeName, lkPhys, lkSize, lkStride);
		if (lkPhys != 0) {
			fPhys = lkPhys;
		}
		if (lkStride >= 1080 * 4) {
			fRowBytes = lkStride;
		}
		if ((lkSize & 0xffff) >= 1080) {
			fWidth = lkSize & 0xffff;
			fHeight = (lkSize >> 16) & 0xffff;
		}

		/*
		 * Phase 8 MDP5 Direct Scanout: Reconfigure active pipe to 32bpp
		 * XRGB/BGRA at fPhys with 4352-byte row stride.
		 */
		fActivePipe[0x00 / 4] = (fHeight << 16) | fWidth;
		fActivePipe[0x04 / 4] = (fHeight << 16) | fWidth;
		fActivePipe[0x08 / 4] = 0;
		fActivePipe[0x0c / 4] = (fHeight << 16) | fWidth;
		fActivePipe[0x10 / 4] = 0;
		fActivePipe[0x14 / 4] = (uint32_t)fPhys;
		fActivePipe[0x24 / 4] = fRowBytes;
		fActivePipe[0x30 / 4] = 0x000236ffu;
		fActivePipe[0x34 / 4] = 0x03020001u;
		__asm__ volatile("dsb sy" ::: "memory");
		IOLog("OP2 FB: reconfigured active pipe %s to 32bpp at 0x%llx (%ux%u stride=%u)\n",
		    pipeName, fPhys, fWidth, fHeight, fRowBytes);
	} else {
		IOLog("OP2 FB: no active pipe found, configuring RGB0 on LM0\n");
		fActivePipe = rgb0;
		fPipeFlushBit = 1u << 3;
		if (rgb0 != NULL) {
			rgb0[0x00 / 4] = (fHeight << 16) | fWidth;
			rgb0[0x04 / 4] = (fHeight << 16) | fWidth;
			rgb0[0x08 / 4] = 0;
			rgb0[0x0c / 4] = (fHeight << 16) | fWidth;
			rgb0[0x10 / 4] = 0;
			rgb0[0x14 / 4] = (uint32_t)fPhys;
			rgb0[0x24 / 4] = fRowBytes;
			rgb0[0x30 / 4] = 0x000236ffu;
			rgb0[0x34 / 4] = 0x03020001u;
		}
		if (fMdpCtl != NULL) {
			fMdpCtl[0x00 / 4] = 1 << 9; /* RGB0 stage BASE on LM0 */
		}
	}

	fLock = IOLockAlloc();
	fClients = OSArray::withCapacity(4);
	fSurface = op2_surface_create_physical(fPhys, fWidth, fHeight, fRowBytes, OP2_FB_FORMAT_BGRA);
	fPixels = fSurface != NULL ? fSurface->kernelAddress() : 0;
	fWorkLoop = IOWorkLoop::workLoop();
	if (fLock == NULL || fClients == NULL || fPixels == 0 || fWorkLoop == NULL) {
		IOLog("OP2 FB: setup failed\n");
		OSSafeReleaseNULL(fSurface);    /* stays published: also held by the table */
		OSSafeReleaseNULL(fWorkLoop);
		return false;
	}
	fVsync = IOTimerEventSource::timerEventSource(this, &AppleCLCD::vsyncFired);
	if (fVsync == NULL || fWorkLoop->addEventSource(fVsync) != kIOReturnSuccess) {
		IOLog("OP2 FB: no vsync timer\n");
		return false;
	}
	uint32_t hz = 60;
	PE_parse_boot_argn("op2fb_hz", &hz, sizeof(hz));
	fFlushTicks = (hz == 0 || hz >= 60) ? 1 : 60 / hz;
	int cui = 1;
	PE_parse_boot_argn("op2fb_client_ui", &cui, sizeof(cui));
	fClientUI = (cui != 0);
	fVsync->setTimeoutUS(OP2_FB_VSYNC_US);

	IOLog("OP2 FB: fPixels=0x%lx fPhys=0x%llx %ux%u stride=%u\n",
	    (unsigned long)fPixels, fPhys, fWidth, fHeight, fRowBytes);

	/* Render authentic Apple boot splash: pitch-black background with white Apple logo & progress bar */
	if (fPixels != 0) {
		uint8_t *fb = (uint8_t *)fPixels;

		/* 1. Deep pitch-black background */
		bzero(fb, (size_t)fHeight * fRowBytes);

		/* 2. Official Apple Retina Boot Logo from ProgressUI (194x237 @3x) */
		int32_t lx0 = (int32_t)(fWidth / 2) - (APPLE_LOGO_WIDTH / 2);   /* 540 - 97 = 443 */
		int32_t ly0 = 850 - (APPLE_LOGO_HEIGHT / 2);                    /* 850 - 118 = 732 */

		for (int32_t ly = 0; ly < APPLE_LOGO_HEIGHT; ly++) {
			int32_t y = ly0 + ly;
			if (y < 0 || y >= (int32_t)fHeight) continue;
			uint32_t *row = (uint32_t *)(void *)(fb + (uint32_t)y * fRowBytes);
			const uint8_t *alpha_row = &apple_logo_alpha[ly * APPLE_LOGO_WIDTH];
			for (int32_t lx = 0; lx < APPLE_LOGO_WIDTH; lx++) {
				uint8_t a = alpha_row[lx];
				if (a != 0) {
					int32_t x = lx0 + lx;
					if (x >= 0 && x < (int32_t)fWidth) {
						row[x] = (0xFFu << 24) | ((uint32_t)a << 16) | ((uint32_t)a << 8) | (uint32_t)a;
					}
				}
			}
		}

		/* 3. Authentic iOS Progress Bar: initialize animator at 0% */
		fBootStartMachTime = mach_absolute_time();
		fLastProgressPct = 0;
		fUserlandTookOver = false;
		drawProgressBar(0);
		__asm__ volatile("dsb sy" ::: "memory");
		IOLog("OP2 FB: authentic Apple boot splash rendered to framebuffer (0x%llx)\n", fPhys);
	}

	/* Kick command-mode panel to replace the LK splash logo with iOS boot screen */
	op2_fb_kick_panel(fMdpCtl);
	delay(20000); /* 20 ms to allow initial frame transfer to finish cleanly */
	IOLog("OP2 FB: initial panel kickoff complete (kicks=%llu)\n", op2_fb_kick_count);

	setProperty(kIOUserClientClassKey, "IOMobileFramebufferUserClient");
	setProperty("external", false);
	setProperty("DisplayPipePlaneBaseAlignment", 64ULL, 32);
	registerService();
	IOLog("OP2 FB: AppleCLCD %ux%u stride %u at 0x%llx, default surface %u, flush every %u vsyncs, client-ui %s\n",
	    fWidth, fHeight, fRowBytes, fPhys, fSurface->id, fFlushTicks, fClientUI ? "on" : "off");
	op2_fb_instance = this;

	/*
	 * Phase 1 Metal Bring-Up: Publish IOAcceleratorES service.
	 * CoreAnimation and Metal.framework discover graphics accelerators via
	 * IOServiceMatching("IOAcceleratorES") and IOServiceMatching("AGXAccelerator").
	 * When boot-arg metal=N is present (e.g. metal=1, metal=2), we set
	 * MetalPluginName and MetalPluginClassName so userland loads OP2Metal.bundle.
	 * By default (metal=0), no accelerator is published so CoreAnimation uses
	 * its native, fully-functional CPU software rasterizer.
	 */
	uint32_t metal = 0;
	PE_parse_boot_argn("metal", &metal, sizeof(metal));
	if (metal > 0) {
		AGXAccelerator *accelerator = OSTypeAlloc(AGXAccelerator);
		if (accelerator != NULL) {
			if (accelerator->init() && accelerator->attach(this)) {
				accelerator->setName("sgx");
				accelerator->setProperty("MetalPluginName", "OP2Metal");
				accelerator->setProperty("MetalPluginClassName", "OP2MetalDevice");
				accelerator->setProperty("IOGLBundleName", "OP2Metal");
				accelerator->setProperty("IOGLPluginName", "OP2Metal");
				accelerator->setProperty("IOGLESBundleName", "AGXGLDriver");
				accelerator->setProperty("IOGLESMetalBundleName", "AGXGLDriver");
				IOLog("OP2 FB: metal=%u: published IOAcceleratorES/AGXAccelerator naming OP2Metal\n", metal);
				accelerator->setProperty(kIOUserClientClassKey, "OP2GPUUserClient");
				accelerator->registerService();
			}
			accelerator->release();
		}
	} else {
		IOLog("OP2 FB: metal disabled; using native CPU software rasterization\n");
	}

	return true;
}

void
AppleCLCD::vsyncFired(OSObject *owner, IOTimerEventSource *sender)
{
	AppleCLCD *fb = (AppleCLCD *)owner;
	uint64_t now = mach_absolute_time();
	uint64_t count = ++fb->fVsyncCount;
	uint64_t values[6] = {
		(uint32_t)count, (uint32_t)(count >> 32),
		(uint32_t)now, (uint32_t)(now >> 32),
		0, 0,
	};

	/* 60s display timeline: WHEN does the panel go black (timer vs event).
	 * ~20 lines/run; survives the ring. */
	if ((count % 3600) == 0 && op2_fb_instance != NULL &&
	    op2_fb_instance->fPixels != 0) {
		uint32_t tl_nz = 0, tl_n = 0;
		const uint32_t *px = (const uint32_t *)op2_fb_instance->fPixels;
		uint64_t total = (uint64_t)op2_fb_instance->fWidth *
		    (uint64_t)op2_fb_instance->fHeight;
		for (uint64_t i = 0; i < total && tl_n < 2000; i += 997) {
			if ((px[i] & 0x00FFFFFFu) != 0) {
				tl_nz++;
			}
			tl_n++;
		}
		IOLog("OP2 FB TL min%llu: presents=%llu kicks=%llu shown=%u panel_nz=%u\n",
		    count / 3600, op2_fb_present_count, op2_fb_kick_count,
		    op2_fb_last_shown, tl_nz);
	}

	/* Animate authentic Apple boot progress bar smoothly until real userland UI takes over */
	if (!fb->fUserlandTookOver && fb->fPixels != 0) {
		/* MSM8994 counter runs at 19.2 MHz (19200 ticks per ms) */
		uint64_t elapsed_ticks = now - fb->fBootStartMachTime;
		uint32_t elapsed_ms = (uint32_t)(elapsed_ticks / 19200);
		uint32_t target_pct = 0;
		if (elapsed_ms < 3000) {
			target_pct = (elapsed_ms * 35) / 3000;
		} else if (elapsed_ms < 8000) {
			target_pct = 35 + ((elapsed_ms - 3000) * 35) / 5000;
		} else if (elapsed_ms < 15000) {
			target_pct = 70 + ((elapsed_ms - 8000) * 25) / 7000;
		} else if (elapsed_ms < 20000) {
			target_pct = 95 + ((elapsed_ms - 15000) * 5) / 5000;
		} else {
			target_pct = 100;
		}

		if (target_pct > fb->fLastProgressPct) {
			fb->fLastProgressPct = target_pct;
			fb->drawProgressBar(target_pct);
		}
	}

	IOLockLock(fb->fLock);
	for (unsigned int i = 0; i < fb->fClients->getCount(); i++) {
		IOMobileFramebufferUserClient *client =
		    (IOMobileFramebufferUserClient *)fb->fClients->getObject(i);
		client->notify(OP2_FB_NOTIFY_VSYNC, values, 6);
	}
	bool flush = fb->fDirty && (count % fb->fFlushTicks) == 0;
	uint8_t *rec = NULL;
	if (flush) {
		rec = (uint8_t *)IOMallocData(OP2_FB_SWAP_REC_SIZE);
		if (rec != NULL) {
			memcpy(rec, fb->fPending, OP2_FB_SWAP_REC_SIZE);
			fb->fDirty = false;
		}
	}
	IOLockUnlock(fb->fLock);
	if (rec != NULL) {
		fb->present(rec);
		IOFreeData(rec, OP2_FB_SWAP_REC_SIZE);
	}
	sender->setTimeoutUS(OP2_FB_VSYNC_US);
}

void
AppleCLCD::addClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	fClients->setObject(client);
	IOLockUnlock(fLock);
}

void
AppleCLCD::removeClient(IOMobileFramebufferUserClient *client)
{
	IOLockLock(fLock);
	unsigned int i = fClients->getNextIndexOfObject(client, 0);
	if (i != (unsigned int)-1) {
		fClients->removeObject(i);
	}
	IOLockUnlock(fLock);
}

static uint32_t
get32(const uint8_t *p, uint32_t off)
{
	uint32_t v;
	memcpy(&v, p + off, 4);
	return v;
}

/*
 * Strided non-zero sample of a surface's pixels (every 8th pixel, capped).
 * Tells "userland renders blanks" apart from "kernel copies the wrong
 * memory". Fully bounds-checked; safe for any userland-created surface.
 */
static uint32_t
op2_fb_sample_nz(OP2Surface *s, uint32_t cap)
{
	uint32_t nz = 0;
	if (s == NULL) {
		return 0;
	}
	uintptr_t psrc = s->kernelAddress();
	uint32_t pbpe = s->bytesPerElement ? s->bytesPerElement : 4;
	if (psrc == 0 || pbpe != 4 || s->memory == NULL ||
	    s->width == 0 || s->height == 0 ||
	    (uint64_t)s->bytesPerRow < (uint64_t)s->width * 4 ||
	    (uint64_t)s->offset + (uint64_t)s->height * s->bytesPerRow > s->memory->getLength()) {
		return 0;
	}
	for (uint32_t y = 0; y < s->height && nz <= cap; y += 8) {
		const uint32_t *row = (const uint32_t *)(psrc + s->offset +
		    (uint64_t)y * s->bytesPerRow);
		for (uint32_t x = 0; x < s->width; x += 8) {
			if ((row[x] & 0x00ffffff) != 0 && ++nz > cap) {
				break;
			}
		}
	}
	return nz;
}

static uint64_t op2_fb_swap_count;
static uint64_t op2_fb_present_no_target;

/*
 * Swap records the latest frame submitted by userland (IOMobileFramebufferSwapEnd).
 */
void
AppleCLCD::swap(const uint8_t *rec)
{
	op2_fb_swap_count++;
	if (op2_fb_swap_count <= 8 || (op2_fb_swap_count % 100) == 0) {
		IOLog("OP2 FB: swap %llu (id40=%u, s44=%u, maskC8=0x%x)\n",
		    op2_fb_swap_count, get32(rec, 0x40),
		    get32(rec, 0x44), get32(rec, 0xc8));
	}

	IOLockLock(fLock);
	memcpy(fPending, rec, OP2_FB_SWAP_REC_SIZE);
	fDirty = true;
	IOLockUnlock(fLock);
}

/*
 * Present active swap layers to the physical panel memory.
 *
 * In iOS 12 (iPhone 6 Plus, n56ap, 16H81), the swap record structure passed to
 * IOMobileFramebufferSwapEnd (selector 5, size 0x278) layout is:
 *   rec + 0x40: swap ID
 *   rec + 0x44: layer 0 surface ID (fallback 0x54)
 *   rec + 0x48: layer 1 surface ID (fallback 0x58)
 *   rec + 0x4c: layer 2 surface ID (fallback 0x5c)
 *   rec + 0x50: layer 0 source rect (int sx, sy, w, h)
 *   rec + 0xc8: layer enable bitmask (fallback 0x104)
 */
void
AppleCLCD::present(const uint8_t *rec)
{
	uint64_t pc = ++op2_fb_present_count;
	memcpy(op2_fb_last_rec, rec, sizeof(op2_fb_last_rec));
	op2_fb_last_pc = pc;
	if (!fConsolePaused) {
		int quiet = 0;
		fConsolePaused = true;
		if (PE_parse_boot_argn("op2fb_noconsole", &quiet, sizeof(quiet)) && quiet != 0) {
			PE_Video video;
			PE_current_console(&video);
			PE_initialize_console(&video, kPEDisableScreen);
		}
	}

	uint32_t mask = get32(rec, 0xc8);
	if (mask == 0) {
		mask = get32(rec, 0x104);
	}

	uint32_t target_id = 0;
	/* Check layers 0..2 for active surface */
	for (uint32_t l = 0; l < 3; l++) {
		if (mask & (1u << l)) {
			uint32_t id = get32(rec, 0x44 + 4 * l);
			if (id == 0) {
				id = get32(rec, 0x54 + 4 * l);
			}
			if (id >= 2 && id < OP2_SURFACE_MAX_ID) {
				target_id = id;
				break;
			}
		}
	}

	static uint32_t sLastPresentedID = 0;

	/* Fallback: if swap record had no surface ID specified, maintain previous active surface */
	if (target_id == 0 && sLastPresentedID != 0) {
		target_id = sLastPresentedID;
	}

	if (target_id == 0 && fClientUI) {
		/*
		 * GPU-less or early swap policy: if swap record had no target
		 * surface, scan for an active client surface with non-zero pixels.
		 */
		OP2Surface *best = NULL;
		uint32_t best_nz = 100;
		uint32_t highest_fullscreen_id = 0;
		for (uint32_t sid = 2; sid < 64; sid++) {
			OP2Surface *cand = op2_surface_copy(sid);
			if (cand == NULL) {
				continue;
			}
			if (cand->width >= 1000 && cand->height >= 1000) {
				highest_fullscreen_id = cand->id;
				uint32_t nz = op2_fb_sample_nz(cand, 5000);
				if (nz > best_nz) {
					best_nz = nz;
					if (best != NULL) {
						best->release();
					}
					best = cand;
					cand = NULL;
				}
			}
			if (cand != NULL) {
				cand->release();
			}
		}
		if (best != NULL) {
			target_id = best->id;
			best->release();
		} else if (highest_fullscreen_id != 0) {
			target_id = highest_fullscreen_id;
		}
	}

	if (target_id == 0) {
		/* No layer resolved: mask parsing may be wrong for this userland. */
		if (++op2_fb_present_no_target <= 4 || (op2_fb_present_no_target % 60) == 0) {
			IOLog("OP2 FB: present #%llu no target (mask 0x%x id44=%u id48=%u id4c=%u)\n",
			    pc, mask, get32(rec, 0x44), get32(rec, 0x48), get32(rec, 0x4c));
		}
		return;
	}

	OP2Surface *s = (target_id == (fSurface ? fSurface->id : 1)) ? fSurface : op2_surface_copy(target_id);
	if (s == NULL) {
		return;
	}

	if (target_id != sLastPresentedID) {
		sLastPresentedID = target_id;
		IOLog("OP2 FB: *** DISPLAY SWITCHED TO SURFACE %u (%ux%u) ***\n",
		    target_id, s->width, s->height);
	}

	if (!fUserlandTookOver && target_id >= 2) {
		fUserlandTookOver = true;
		IOLog("OP2 FB: *** DISPLAY HAND-OFF TO USERLAND (surface %u, %ux%u) ***\n",
		    target_id, s->width, s->height);
	}

	uint32_t shown_id = target_id;
	if (fClientUI && s != fSurface && op2_fb_sample_nz(s, 5000) == 0) {
		/*
		 * GPU-less policy (boot-arg op2fb_client_ui=1): the render
		 * server cannot composite without a GPU, so its target stays
		 * blank. Clients still rasterize into their own full-screen
		 * surfaces on CPU; show the fullest one instead of a black
		 * frame. Never triggers on a healthy (non-blank) server.
		 */
		OP2Surface *best = NULL;
		uint32_t best_nz = 100; /* minimum content threshold */
		for (uint32_t sid = 2; sid < 64; sid++) {
			if (sid == target_id) {
				continue;
			}
			OP2Surface *cand = op2_surface_copy(sid);
			if (cand == NULL) {
				continue;
			}
			if (cand->width >= 1000 && cand->height >= 1000) {
				uint32_t nz = op2_fb_sample_nz(cand, 5000);
				if (nz > best_nz) {
					best_nz = nz;
					if (best != NULL) {
						best->release();
					}
					best = cand;
					cand = NULL;
				}
			}
			if (cand != NULL) {
				cand->release();
			}
		}
		if (best != NULL) {
			s->release(); /* s is a copy here, never fSurface */
			s = best;
			shown_id = best->id;
		}
	}
	static uint32_t sLastShown = 0;
	if (shown_id != sLastShown) {
		sLastShown = shown_id;
		op2_fb_last_shown = shown_id;
		IOLog("OP2 FB: now showing surface %u (swap target %u)\n",
		    shown_id, target_id);
	}

	uint32_t shown_nz = op2_fb_sample_nz(s, 5000);
	op2_fb_last_nz = shown_nz;

	if (pc <= 4 || (pc % 60) == 0) {
		/* Rate-limited to protect the 256 KB ramoops window. */
		IOLog("OP2 FB: present #%llu surf %u (%ux%u bpr %u) mask 0x%x shown %u nz~%u\n",
		    pc, target_id, s->width, s->height, s->bytesPerRow, mask, shown_id, shown_nz);
	}

	static bool sHasNonZeroUI = false;
	if (shown_nz > 0) {
		sHasNonZeroUI = true;
	} else {
		/*
		 * Incoming frame has no visible RGB pixels.
		 * If we haven't seen a non-zero frame yet, preserve the Apple boot splash!
		 * If we've already seen a non-zero frame, preserve the active UI!
		 * Under NO circumstances should an all-zero buffer be scanned out or copied to the panel.
		 */
		static uint32_t sPreserveLog = 0;
		if (++sPreserveLog <= 5 || (sPreserveLog % 60) == 0) {
			IOLog("OP2 FB: preserving display (surface %u has no RGB pixels, non_zero=%d)\n",
			    shown_id, (int)sHasNonZeroUI);
		}
		if (s != fSurface) {
			s->release();
		}
		return;
	}

	uint64_t direct_phys = 0;
	IOPhysicalLength segLen = 0;
	if (s->memory != NULL) {
		direct_phys = (uint64_t)s->memory->getPhysicalSegment(0, &segLen);
	}
	op2_fb_last_direct_phys = direct_phys;

	if (direct_phys != 0 && segLen >= (uint64_t)s->bytesPerRow * s->height &&
	    s->width == fWidth && s->height == fHeight && fActivePipe != NULL) {
		/*
		 * Phase 8 ZERO-COPY HARDWARE DIRECT SCANOUT:
		 * Surface is physically contiguous in hardware DMA memory.
		 * Point MDP5 hardware pipe directly to the surface's physical address.
		 */
		static uint64_t s_last_direct_phys = 0;
		if (direct_phys != s_last_direct_phys) {
			s_last_direct_phys = direct_phys;
			fActivePipe[0x14 / 4] = (uint32_t)direct_phys;
			fActivePipe[0x24 / 4] = s->bytesPerRow;
			fActivePipe[0x00 / 4] = (s->height << 16) | s->width;
			fActivePipe[0x04 / 4] = (s->height << 16) | s->width;
			fActivePipe[0x30 / 4] = 0x000236ffu;
			fActivePipe[0x34 / 4] = 0x03020001u;
			static uint32_t sDirectScanoutLogged = 0;
			if (++sDirectScanoutLogged <= 5 || (sDirectScanoutLogged % 60) == 0) {
				IOLog("OP2 FB: direct hardware scanout surf %u at 0x%llx (%ux%u stride %u, switch #%u)\n",
				    shown_id, direct_phys, s->width, s->height, s->bytesPerRow, sDirectScanoutLogged);
			}
		}
	} else {
		/* Fallback: fast 32bpp memory copy / downsample into fPixels at fPhys */
		if (fActivePipe != NULL && fActivePipe[0x14 / 4] != (uint32_t)fPhys) {
			fActivePipe[0x14 / 4] = (uint32_t)fPhys;
			fActivePipe[0x24 / 4] = fRowBytes;
			fActivePipe[0x00 / 4] = (fHeight << 16) | fWidth;
			fActivePipe[0x04 / 4] = (fHeight << 16) | fWidth;
			fActivePipe[0x30 / 4] = 0x000236ffu;
			fActivePipe[0x34 / 4] = 0x03020001u;
		}

		uintptr_t src = s->kernelAddress();
		uint32_t bpe = s->bytesPerElement ? s->bytesPerElement : 4;
		if (src != 0 && bpe == 4 && fPixels != 0) {
			if (s->width == 1242 && s->height == 2208) {
				static uint32_t sy_map[1920];
				static bool maps_ready = false;
				if (!maps_ready) {
					for (int32_t y = 0; y < 1920; y++) {
						sy_map[y] = (uint32_t)(((uint64_t)y * 2208) / 1920);
					}
					maps_ready = true;
				}
				uint64_t last_retina = (uint64_t)s->offset + (uint64_t)2207 * s->bytesPerRow + (uint64_t)1242 * 4;
				if (s->memory != NULL && last_retina <= s->memory->getLength()) {
					uint64_t t0 = mach_absolute_time();
					for (int32_t y = 0; y < (int32_t)fHeight; y++) {
						uint32_t sy_coord = sy_map[y];
						const uint32_t *src_row = (const uint32_t *)(src + s->offset + (uint64_t)sy_coord * s->bytesPerRow);
						uint32_t *dst_row = (uint32_t *)(fPixels + (uint64_t)y * fRowBytes);

						/*
						 * High-Speed 20-pixel Block Downsampler (1242 -> 1080):
						 * 1080 = 54 blocks of 20 pixels; 1242 = 54 blocks of 23 pixels.
						 * Block pattern: 7 contiguous pixels, skip 1, 7 contiguous pixels, skip 1, 6 contiguous pixels, skip 2.
						 * Aligned 64-bit pair loads/stores with zero lookup table overhead.
						 */
						for (int32_t b = 0; b < 54; b++) {
							const uint32_t *sp = src_row + b * 23;
							uint32_t *dp = dst_row + b * 20;

							const uint64_t *sp64 = (const uint64_t *)(uintptr_t)sp;
							uint64_t *dp64 = (uint64_t *)(uintptr_t)dp;

							/* Pixels 0..5 (3x 64-bit) + pixel 6 */
							dp64[0] = sp64[0];
							dp64[1] = sp64[1];
							dp64[2] = sp64[2];
							dp[6]   = sp[6];

							/* Pixel 7 (skip sp[7]) */
							dp[7]   = sp[8];

							/* Pixels 8..13 (3x 64-bit) from sp[9..14] */
							const uint64_t *sp_mid64 = (const uint64_t *)(uintptr_t)(sp + 9);
							uint64_t *dp_mid64 = (uint64_t *)(uintptr_t)(dp + 8);
							dp_mid64[0] = sp_mid64[0];
							dp_mid64[1] = sp_mid64[1];
							dp_mid64[2] = sp_mid64[2];

							/* Pixels 14..19 (3x 64-bit) from sp[16..21] (skip sp[15]) */
							const uint64_t *sp_end64 = (const uint64_t *)(uintptr_t)(sp + 16);
							uint64_t *dp_end64 = (uint64_t *)(uintptr_t)(dp + 14);
							dp_end64[0] = sp_end64[0];
							dp_end64[1] = sp_end64[1];
							dp_end64[2] = sp_end64[2];
							/* sp[22] is skipped */
						}
					}
					uint64_t dt = mach_absolute_time() - t0;
					uint32_t us = (uint32_t)((dt * 10) / 192);
					static uint32_t sDownsampleLogged = 0;
					if (++sDownsampleLogged <= 5 || (sDownsampleLogged % 60) == 0) {
						IOLog("OP2 FB: downsample 1242x2208->1080x1920 took %u us (frame #%u)\n", us, sDownsampleLogged);
					}
				}
			} else {
				int32_t w = (int32_t)s->width;
				int32_t h = (int32_t)s->height;
				if (w > (int32_t)fWidth) w = (int32_t)fWidth;
				if (h > (int32_t)fHeight) h = (int32_t)fHeight;
				uint64_t last = (uint64_t)s->offset + (uint64_t)(h - 1) * s->bytesPerRow + (uint64_t)w * 4;
				if (w > 0 && h > 0 && s->memory != NULL && last <= s->memory->getLength()) {
					uint32_t line_bytes = (uint32_t)w * 4;
					for (int32_t y = 0; y < h; y++) {
						const void *src_line = (const void *)(src + s->offset + (uint64_t)y * s->bytesPerRow);
						void *dst_line = (void *)(fPixels + (uint64_t)y * fRowBytes);
						memcpy(dst_line, src_line, line_bytes);
					}
				}
			}
		}
	}

	if (s != fSurface) {
		s->release();
	}

	/* Memory barrier to ensure all write-combined stores drain to DRAM */
	__asm__ volatile("dsb sy" ::: "memory");

	/* Kick command-mode panel to send newly swapped frame to display GRAM */
	op2_fb_kick_panel(fMdpCtl);
}

/*
 * Display getters for input/digitizer synchronization:
 */
extern "C" uint32_t op2_fb_get_last_shown(void);
extern "C" uint32_t op2_fb_get_last_nz(void);

uint32_t
op2_fb_get_last_shown(void)
{
	return op2_fb_last_shown;
}

uint32_t
op2_fb_get_last_nz(void)
{
	return op2_fb_last_nz;
}

/*
 * Deadline-dump display verdict: proves lit-UI vs black pixels from the log
 * alone (no eyes needed). Samples panel memory directly; lock-free MMIO read.
 */
extern "C" void op2_fb_dump_stats(void);
extern "C" void
op2_fb_dump_stats(void)
{
	/* kprintf-only in here: the deadline dump runs with interrupts
	 * disabled and IOLog asserts there (run177). */
	uint32_t panel_nz = 0, samples = 0;
	uint64_t presents = op2_fb_present_count, kicks = op2_fb_kick_count;
	uint32_t shown = op2_fb_last_shown, w = 0, h = 0;
	uint32_t active_phys = (op2_fb_instance != NULL && op2_fb_instance->fActivePipe != NULL) ?
	    op2_fb_instance->fActivePipe[0x14 / 4] : 0;

	if (op2_fb_instance != NULL && op2_fb_instance->fPixels != 0) {
		w = op2_fb_instance->fWidth;
		h = op2_fb_instance->fHeight;
		const uint32_t *px = (const uint32_t *)op2_fb_instance->fPixels;
		uint64_t total = (uint64_t)w * h;
		for (uint64_t i = 0; i < total && samples < 2000; i += 997) {
			if ((px[i] & 0x00FFFFFFu) != 0) {
				panel_nz++;
			}
			samples++;
		}
	}
	printf("OP2 FB STATS: presents=%llu kicks=%llu shown=%u active_phys=0x%08x direct_phys=0x%08llx panel=%ux%u panel_nz=%u/%u lastnz=%u\n",
	    presents, kicks, shown, active_phys, op2_fb_last_direct_phys, w, h, panel_nz, samples, op2_fb_last_nz);
	/* Output directly via printf to be captured in persistent ramoops buffer */
	printf("OP2 FB LAST begin rec=%llu size=%u\n",
	    op2_fb_last_pc, (uint32_t)sizeof(op2_fb_last_rec));
	for (uint32_t off = 0; off + 16 <= (uint32_t)sizeof(op2_fb_last_rec); off += 16) {
		printf("OP2 FB LAST %u: %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u %u\n",
		    off,
		    op2_fb_last_rec[off+0], op2_fb_last_rec[off+1],
		    op2_fb_last_rec[off+2], op2_fb_last_rec[off+3],
		    op2_fb_last_rec[off+4], op2_fb_last_rec[off+5],
		    op2_fb_last_rec[off+6], op2_fb_last_rec[off+7],
		    op2_fb_last_rec[off+8], op2_fb_last_rec[off+9],
		    op2_fb_last_rec[off+10], op2_fb_last_rec[off+11],
		    op2_fb_last_rec[off+12], op2_fb_last_rec[off+13],
		    op2_fb_last_rec[off+14], op2_fb_last_rec[off+15]);
	}
	printf("OP2 FB LAST end\n");
}

bool
IOMobileFramebufferUserClient::initWithTask(task_t owningTask, void *securityID, UInt32 type)
{
	if (!IOUserClient::initWithTask(owningTask, securityID, type)) {
		return false;
	}
	fTask = owningTask;
	return true;
}

bool
IOMobileFramebufferUserClient::start(IOService *provider)
{
	fFB = OSDynamicCast(AppleCLCD, provider);
	if (fFB == NULL || !IOUserClient::start(provider)) {
		return false;
	}
	fFB->addClient(this);
	return true;
}

IOReturn
IOMobileFramebufferUserClient::clientClose(void)
{
	if (fFB != NULL) {
		fFB->removeClient(this);
		IOLockLock(fFB->fLock);
		mach_port_t old = fPort;
		fPort = MACH_PORT_NULL;
		IOLockUnlock(fFB->fLock);
		if (old != MACH_PORT_NULL) {
			releaseNotificationPort(old);
		}
	}
	terminate();
	return kIOReturnSuccess;
}

IOReturn
IOMobileFramebufferUserClient::registerNotificationPort(mach_port_t port, __unused UInt32 type,
    __unused io_user_reference_t refCon)
{
	mach_port_t old;
	IOLockLock(fFB->fLock);
	old = fPort;
	fPort = port;
	IOLockUnlock(fFB->fLock);
	if (old != MACH_PORT_NULL) {
		releaseNotificationPort(old);
	}
	return kIOReturnSuccess;
}

/* Called with the framebuffer lock held. */
void
IOMobileFramebufferUserClient::notify(uint32_t type, const uint64_t *values, uint32_t count)
{
	OSAsyncReference64 ref;

	if (type >= OP2_FB_NOTIFY_TYPES || fFunc[type] == 0 || fPort == MACH_PORT_NULL) {
		return;
	}
	bzero(ref, sizeof(ref));
	setAsyncReference64(ref, fPort, (mach_vm_address_t)fFunc[type], fRefcon[type], fTask);
	sendAsyncResult64WithOptions(ref, kIOReturnSuccess, (io_user_reference_t *)values, count,
	    kIOUserNotifyOptionCanDrop);
}

static uint64_t op2_fb_logged;        /* selectors already logged (< 64) */

IOReturn
IOMobileFramebufferUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    __unused IOExternalMethodDispatch *dispatch, __unused OSObject *target,
    __unused void *reference)
{
	IOReturn ret = kIOReturnSuccess;
	uint64_t out[2] = { 0, 0 };
	uint32_t outCount = 0;

	if (args->structureOutputDescriptor == NULL && args->structureOutput != NULL) {
		bzero(args->structureOutput, args->structureOutputSize);
	}

	switch (selector) {
	case kOP2FBGetLayerDefaultSurface:
		out[0] = op2_surface_port_in_task(fFB->fSurface, fTask);
		outCount = 1;
		break;
	case kOP2FBSwapBegin:
		IOLockLock(fFB->fLock);
		out[0] = ++fFB->fSwapID;
		IOLockUnlock(fFB->fLock);
		outCount = 1;
		break;
	case kOP2FBSwapEnd: {
		uint8_t rec[OP2_FB_SWAP_REC_SIZE];
		bzero(rec, sizeof(rec));
		if (args->structureInputDescriptor != NULL ||
		    args->structureInputSize < 0x278) {
			ret = kIOReturnBadArgument;
			break;
		}
		uint32_t cplen = args->structureInputSize < sizeof(rec) ? (uint32_t)args->structureInputSize : (uint32_t)sizeof(rec);
		memcpy(rec, args->structureInput, cplen);
		fFB->swap(rec);
		break;
	}
	case kOP2FBGetID:
		out[0] = 1;
		outCount = 1;
		break;
	case kOP2FBGetDisplaySize:
		/*
		 * On iPhone 6 Plus (n56ap), UIKit renders at 3x Retina (1242x2208).
		 * Reporting 1242x2208 matches native n56ap display geometry and
		 * prevents CoreAnimation from dropping [1242x2208] render surfaces.
		 */
		out[0] = 1242;
		out[1] = 2208;
		outCount = 2;
		break;
	case kOP2FBRequestPowerChange:
		if (args->scalarInputCount >= 1) {
			bool off = args->scalarInput[0] == 0;
			static int sKeepOn = -1;
			if (sKeepOn == -1) {
				sKeepOn = 1;
				PE_parse_boot_argn("op2fb_keepon", &sKeepOn, sizeof(sKeepOn));
			}
			if (sKeepOn != 0 && off) {
				IOLog("OP2 FB: ignoring display off (op2fb_keepon active)\n");
				break;
			}
			IOLockLock(fFB->fLock);
			if (off != fFB->fPowerOff) {
				fFB->fPowerOff = off;
				fFB->fFadeStep = 0;
				fFB->fDirty = true;     /* on: redraw the latest frame */
			}
			IOLockUnlock(fFB->fLock);
			IOLog("OP2 FB: display %s\n", off ? "off" : "on");
		}
		break;
	case kOP2FBIsMainDisplay:
		out[0] = 1;
		outCount = 1;
		break;
	case kOP2FBGetDotPitch:
		out[0] = OP2_FB_DOT_PITCH;
		outCount = 1;
		break;
	case kOP2FBSurfaceIsReplaceable:
		out[0] = 1;
		outCount = 1;
		break;
	case kOP2FBSupportedFrameInfo:
		if (args->scalarInputCount >= 1 && args->scalarInput[0] > 0) {
			ret = kIOReturnBadArgument;
			break;
		}
		out[0] = 1; /* 1 mode supported */
		outCount = 1;
		if (args->structureOutput != NULL && args->structureOutputSize >= 64) {
			uint32_t *fi = (uint32_t *)args->structureOutput;
			fi[0] = 1242;                 /* Retina logical display width */
			fi[1] = 2208;                 /* Retina logical display height */
			fi[2] = 60;                   /* refresh rate 60 Hz */
			fi[3] = 0;                    /* flags */
			fi[4] = OP2_FB_FORMAT_BGRA;   /* format */
			fi[5] = 1242 * 4;             /* 4968 */
		}
		break;
	case kOP2FBChangeFrameInfo:
		ret = kIOReturnSuccess;
		break;
	case kOP2FBNotifications:
		if (args->scalarInputCount >= 3 && args->scalarInput[2] < OP2_FB_NOTIFY_TYPES) {
			uint32_t type = (uint32_t)args->scalarInput[2];
			bool changed = (fFunc[type] != args->scalarInput[0]);
			IOLockLock(fFB->fLock);
			fFunc[type] = args->scalarInput[0];
			fRefcon[type] = args->scalarInput[1];
			IOLockUnlock(fFB->fLock);
			if (changed) {
				/* backboardd flaps these thousands of times per
				 * run (type 5 on/off per commit); log the first
				 * 8 so the 260KB ring keeps the TL history. */
				static int op2_fb_note_logged = 0;
				if (op2_fb_note_logged < 8) {
					char name[32];
					op2_fb_note_logged++;
					proc_selfname(name, sizeof(name));
					IOLog("OP2 FB: %s notifications type %u %s\n",
					    name, type,
					    args->scalarInput[0] ? "on" : "off");
				}
			}
		}
		break;
	case kOP2FBSwapWait:
	default:
		break;
	}

	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = i < outCount ? out[i] : 0;
	}
	static uint64_t op2_fb_logged_high = 0;
	bool should_log = false;
	if (selector < 64) {
		if (!(op2_fb_logged & (1ULL << selector))) {
			op2_fb_logged |= 1ULL << selector;
			should_log = true;
		}
	} else if (selector < 128) {
		uint32_t bit = selector - 64;
		if (!(op2_fb_logged_high & (1ULL << bit))) {
			op2_fb_logged_high |= 1ULL << bit;
			should_log = true;
		}
	}
	if (should_log) {
		char name[32];
		proc_selfname(name, sizeof(name));
		IOLog("OP2 FB: %s sel %u in %u/%u out %u/%u -> 0x%x\n", name, selector,
		    args->scalarInputCount, args->structureInputSize,
		    args->scalarOutputCount, args->structureOutputSize, ret);
	}
	return ret;
}
#endif /* ARM64_BOARD_CONFIG_MSM8994 */
