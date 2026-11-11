/*
 * Polled BLSP1 QUP2 I2C master for OnePlus 2 (MSM8994) + Synaptics S3320 probe.
 *
 * Touch controller: Synaptics S3320 (RMI4) at 7-bit addr 0x20 on i2c_2
 * (BLSP1 QUP2, base 0xf9924000, 400 kHz from 19.2 MHz XO-derived core clk).
 * I2C pins GPIO 6 (SDA) / GPIO 7 (SCL), func 3. ATTN on GPIO 96.
 * Reference: OnePlusOSS android_kernel_oneplus_msm8994 (5.1.1):
 *   drivers/i2c/busses/i2c-qup.c (QUP programming),
 *   drivers/clk/qcom/clock-gcc-8994.c + clock-local2.c (GCC CBCR/RCG),
 *   arch/arm64/boot/dts/14049_HW_14 device trees (bus/pins/GPIOs).
 *
 * Stage 1: pinmux + clocks + QUP init + RMI4 PDT/F01/F11 probe, all polled
 * (no interrupts). Evidence goes to the kernel log (ramoops).
 */

#include <pexpert/arm64/board_config.h>

#if defined(ARM64_BOARD_CONFIG_MSM8994)
#include <IOKit/IOService.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOLib.h>
#include <kern/clock.h>
#include <kern/thread.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" void op2_digitizer_post(uint32_t touchDown, uint32_t x_fp16, uint32_t y_fp16, uint32_t z_raw);
extern "C" void op2_home_button_post(uint32_t down);

/* Physical bases (MSM8994) */
#define OP2_QUP2_PHYS           0xf9924000ULL
#define OP2_QUP2_SIZE           0x1000
#define OP2_GCC_PHYS            0xfc400000ULL
#define OP2_GCC_SIZE            0x2000
#define OP2_TLMM_PHYS           0xfd510000ULL
#define OP2_TLMM_SIZE           0x4000

/* GCC registers (clock-gcc-8994.c) */
#define OP2_GCC_BLSP1_AHB_CBCR          0x05C4
#define OP2_GCC_BLSP1_QUP2_I2C_CBCR     0x06C8
#define OP2_GCC_BLSP1_QUP2_I2C_CMD_RCGR 0x06E0
#define OP2_GCC_BLSP1_QUP2_I2C_CFG_RCGR 0x06E4
#define OP2_GCC_CBCR_ENABLE             (1U << 0)
#define OP2_GCC_CBCR_CLK_OFF            (1U << 31)

/* TLMM (pinctrl-msm-tlmm.c, 8974 family: gp offset 0x1000, stride 0x10) */
#define OP2_TLMM_GP_OFF(pin)    (0x1000 + (pin) * 0x10)
#define OP2_TLMM_CFG(pin)       (OP2_TLMM_GP_OFF(pin) + 0x0)
#define OP2_TLMM_INOUT(pin)     (OP2_TLMM_GP_OFF(pin) + 0x4)
#define OP2_TLMM_CFG_FUNC_SHFT  2
#define OP2_TLMM_CFG_DRV_SHFT   6

/* QUP registers (i2c-qup.c) */
enum {
	kQupConfig       = 0x00,
	kQupState        = 0x04,
	kQupIoMode       = 0x08,
	kQupSwReset      = 0x0C,
	kQupOperational  = 0x18,
	kQupErrorFlags   = 0x1C,
	kQupErrorFlagsEn = 0x20,
	kQupOutFifoCnt   = 0x10C,
	kQupOutFifoBase  = 0x110,
	kQupMxReadCnt    = 0x208,
	kQupInFifoCnt    = 0x214,
	kQupInFifoBase   = 0x218,
	kQupI2cClkCtl    = 0x400,
	kQupI2cStatus    = 0x404,
};

enum {
	kQupResetState   = 0,
	kQupRunState     = 1,
	kQupPauseState   = 3,
	kQupStateMask    = 3,
	kQupStateValid   = 1U << 2,
	kQupOpReset      = 0xFF0,
	kQupI2cStatReset = 0xFFFFFC,
	kQupOutSvc       = 1U << 8,
	kQupInSvc        = 1U << 9,
	kQupMxInputDone  = 1U << 11,
	kQupPackEn       = 1U << 15,
	kQupUnpackEn     = 1U << 14,
	kQupMiniCoreI2c  = 2U << 8,
	kQupNVal         = 0xF,
	kQupOutStart     = 1U << 8,
	kQupOutData      = 2U << 8,
	kQupOutStop      = 3U << 8,
	kQupOutRec       = 4U << 8,
	kQupI2cNack      = 1U << 3,
	kQupInNotEmpty   = 1U << 5,
	kQupErrEn        = 0x7C,
	kI2cStatBusActive = 1U << 8,
	kI2cStatErrMask  = 0x38000FC,
};

/* 400 kHz from 19.2 MHz: fs_div = (19200000/400000/2)-3 = 21, hs_div = 3 */
#define OP2_QUP_CLK_CTL_400K    ((3U << 8) | 21U)
#define OP2_SYNA_ADDR           0x20

class OP2I2C : public IOService {
	OSDeclareDefaultStructors(OP2I2C);

	volatile uint32_t *fQup;
	volatile uint32_t *fGcc;
	volatile uint32_t *fTlmm;

	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;

	static inline uint32_t rd(volatile uint32_t *b, uint32_t off);
	static inline void wr(volatile uint32_t *b, uint32_t off, uint32_t v);
	bool gccBranchOn(uint32_t cbcr);
	bool qupPollState(uint32_t want);
	bool qupSetState(uint32_t state);
	void qupReset(void);
	/* Combined write-then-read (repeated START, no STOP between).
	 * Returns 0 on success, 1 on NACK (no device), -1 on HW error. */
	int qupWrRd(uint8_t addr, const uint8_t *wbuf, uint32_t wlen,
	    uint8_t *rbuf, uint32_t rlen);
	int synaRead(uint8_t reg, uint8_t *buf, uint32_t len);
	int synaWrite(uint8_t reg, uint8_t val);
	void synaProbe(void);
	bool synaInit(void);
	void pollLoop(void) __attribute__((noreturn));
	static void pollThread(void *param, wait_result_t res) __attribute__((noreturn));
};

OSDefineMetaClassAndStructors(OP2I2C, IOService);

uint32_t
OP2I2C::rd(volatile uint32_t *b, uint32_t off)
{
	__asm__ volatile("dsb sy" ::: "memory");
	return b[off / 4];
}

void
OP2I2C::wr(volatile uint32_t *b, uint32_t off, uint32_t v)
{
	b[off / 4] = v;
	__asm__ volatile("dsb sy" ::: "memory");
}

bool
OP2I2C::gccBranchOn(uint32_t cbcr)
{
	uint32_t v = rd(fGcc, cbcr);
	IOLog("OP2I2C: GCC CBCR 0x%x before 0x%08x\n", cbcr, v);
	wr(fGcc, cbcr, v | OP2_GCC_CBCR_ENABLE);
	for (int i = 0; i < 1000; i++) {
		v = rd(fGcc, cbcr);
		if ((v & OP2_GCC_CBCR_CLK_OFF) == 0) {
			IOLog("OP2I2C: GCC CBCR 0x%x on (0x%08x)\n", cbcr, v);
			return true;
		}
		IOSleep(1);
	}
	IOLog("OP2I2C: GCC CBCR 0x%x stuck OFF (0x%08x)\n", cbcr, v);
	return false;
}

bool
OP2I2C::qupPollState(uint32_t want)
{
	for (int i = 0; i < 2000; i++) {
		uint32_t s = rd(fQup, kQupState);
		if ((s & kQupStateValid) && ((s & kQupStateMask) == want)) {
			return true;
		}
		if ((i & 0xFF) == 0xFF) {
			IOSleep(1);
		}
	}
	return false;
}

bool
OP2I2C::qupSetState(uint32_t state)
{
	wr(fQup, kQupState, state);
	return qupPollState(state);
}

void
OP2I2C::qupReset(void)
{
	wr(fQup, kQupSwReset, 1);
	qupPollState(kQupResetState);
	wr(fQup, kQupConfig, 0);
	wr(fQup, kQupOperational, kQupOpReset);
	wr(fQup, kQupErrorFlagsEn, kQupErrEn);
	wr(fQup, kQupConfig, kQupMiniCoreI2c | kQupNVal);
	wr(fQup, kQupI2cClkCtl, 0);
	wr(fQup, kQupI2cStatus, kQupI2cStatReset);
}

int
OP2I2C::qupWrRd(uint8_t addr, const uint8_t *wbuf, uint32_t wlen,
    uint8_t *rbuf, uint32_t rlen)
{
	uint16_t entries[40];
	uint32_t n = 0;

	if (wlen + rlen + 4 > sizeof(entries) / sizeof(entries[0])) {
		return -1;
	}
	/* Write phase: START+W, DATA..., last DATA (rd follows) or STOP */
	entries[n++] = (uint16_t)(kQupOutStart | (uint16_t)(addr << 1));
	for (uint32_t i = 0; i < wlen; i++) {
		uint16_t tag = kQupOutData;
		if (i == wlen - 1 && rlen == 0) {
			tag = kQupOutStop;
		}
		entries[n++] = (uint16_t)(tag | wbuf[i]);
	}
	/* Read phase: START+R, REC+len */
	if (rlen > 0) {
		entries[n++] = (uint16_t)(kQupOutStart | (uint16_t)(addr << 1) | 1);
		entries[n++] = (uint16_t)(kQupOutRec | (rlen & 0xFF));
	}

	qupReset();
	wr(fQup, kQupIoMode, kQupPackEn | kQupUnpackEn);
	if (rlen > 0) {
		wr(fQup, kQupMxReadCnt, rlen);
	}
	if (!qupSetState(kQupRunState)) {
		IOLog("OP2I2C: RUN(1) failed, state 0x%x\n",
		    rd(fQup, kQupState));
		return -1;
	}
	wr(fQup, kQupI2cClkCtl, OP2_QUP_CLK_CTL_400K);
	if (!qupSetState(kQupPauseState)) {
		IOLog("OP2I2C: PAUSE failed, state 0x%x\n",
		    rd(fQup, kQupState));
		return -1;
	}
	/* Fill OUT FIFO: two 16-bit entries per 32-bit write, low first */
	for (uint32_t i = 0; i < n; i += 2) {
		uint32_t w = entries[i];
		if (i + 1 < n) {
			w |= (uint32_t)entries[i + 1] << 16;
		}
		wr(fQup, kQupOutFifoBase, w);
	}
	if (!qupSetState(kQupRunState)) {
		IOLog("OP2I2C: RUN(2) failed, state 0x%x\n",
		    rd(fQup, kQupState));
		return -1;
	}

	/* Poll completion: service flags or bus idle; watch error regs */
	uint32_t want = (rlen > 0) ? (kQupMxInputDone | kQupInSvc) : kQupOutSvc;
	bool done = false;
	uint32_t istatus = 0, estatus = 0, op = 0;
	for (int i = 0; i < 500; i++) {
		istatus = rd(fQup, kQupI2cStatus);
		estatus = rd(fQup, kQupErrorFlags);
		op = rd(fQup, kQupOperational);
		if (istatus & kI2cStatErrMask) {
			break;
		}
		if (estatus & 0x7F) {
			break;
		}
		if (op & want) {
			done = true;
			break;
		}
		IOSleep(1);
	}
	/* Ack (W1C) service flags */
	wr(fQup, kQupOperational, kQupOutSvc | kQupInSvc);
	if (istatus & kI2cStatErrMask) {
		IOLog("OP2I2C: I2C_STATUS error 0x%08x (NACK=%d)\n",
		    istatus, (estatus & kQupI2cNack) != 0);
		return (estatus & kQupI2cNack) ? 1 : -1;
	}
	if (estatus & 0x7F) {
		IOLog("OP2I2C: QUP ERROR_FLAGS 0x%08x (NACK=%d)\n",
		    estatus, (estatus & kQupI2cNack) != 0);
		return (estatus & kQupI2cNack) ? 1 : -1;
	}
	if (!done) {
		IOLog("OP2I2C: timeout op=0x%x istatus=0x%x\n", op, istatus);
		return -1;
	}

	/* Drain IN FIFO (UNPACK strips tags; 2 bytes per word) */
	if (rlen > 0) {
		uint32_t pos = 0;
		uint32_t dval = 0;
		for (uint32_t i = 0; i < rlen; i++) {
			if ((i & 1) == 0) {
				if ((rd(fQup, kQupOperational) & kQupInNotEmpty) == 0) {
					IOLog("OP2I2C: IN FIFO underrun at %u/%u\n",
					    i, rlen);
					return -1;
				}
				dval = rd(fQup, kQupInFifoBase);
				rbuf[pos++] = (uint8_t)(dval & 0xFF);
			} else {
				rbuf[pos++] = (uint8_t)((dval >> 16) & 0xFF);
			}
		}
	}
	return 0;
}

int
OP2I2C::synaRead(uint8_t reg, uint8_t *buf, uint32_t len)
{
	return qupWrRd(OP2_SYNA_ADDR, &reg, 1, buf, len);
}

int
OP2I2C::synaWrite(uint8_t reg, uint8_t val)
{
	uint8_t w[2] = { reg, val };
	return qupWrRd(OP2_SYNA_ADDR, w, 2, NULL, 0);
}

/* S3320 register map (fixed, from synaptics_driver_s3320.c) */
enum {
	kSynaF01Data = 0x04, /* data0=status, data1=irq flags */
	kSynaF01Ctrl = 0x0E, /* ctrl0=device control */
	kSynaF01Cmd  = 0x26, /* cmd0: 0x01 = soft reset */
	kSynaF12Data = 0x06, /* 8 bytes/finger, 10 fingers */
	kSynaF12Irq  = 0x04, /* F12 interrupt flag bit in data01 */
};

bool
OP2I2C::synaInit(void)
{
	/* Device control: normal operation + nosleep + configured (0x84).
	 * Writing the configured bit clears "reset occurred" status. */
	uint8_t ctrl0 = 0;
	if (synaRead(kSynaF01Ctrl, &ctrl0, 1) != 0) {
		IOLog("OP2I2C: init: ctrl0 read failed\n");
		return false;
	}
	uint8_t mode = (uint8_t)((ctrl0 & 0xF8) | 0x84);
	if (synaWrite(kSynaF01Ctrl, mode) != 0) {
		IOLog("OP2I2C: init: mode write failed\n");
		return false;
	}
	/* Clear pending interrupts, then enable all sources (0x7F) */
	uint8_t clr = 0;
	if (synaRead((uint8_t)(kSynaF01Data + 1), &clr, 1) != 0) {
		IOLog("OP2I2C: init: irq clear failed\n");
		return false;
	}
	if (synaWrite((uint8_t)(kSynaF01Ctrl + 1), 0x7F) != 0) {
		IOLog("OP2I2C: init: irq enable failed\n");
		return false;
	}
	uint8_t st[2] = { 0xA5, 0xA5 };
	if (synaRead(kSynaF01Data, st, 2) != 0) {
		IOLog("OP2I2C: init: status re-read failed\n");
		return false;
	}
	uint32_t attn = rd(fTlmm, OP2_TLMM_INOUT(96)) & 1;
	IOLog("OP2I2C: init done: ctrl0 %02x->%02x status=%02x irq=%02x ATTN=%u (want status 00 ATTN 1)\n",
	    ctrl0, mode, st[0], st[1], attn);
	return st[0] == 0;
}

void
OP2I2C::pollThread(void *param, wait_result_t res)
{
	(void)res;
	((OP2I2C *)param)->pollLoop();
}

void
OP2I2C::pollLoop(void)
{
	bool prevDown = false;
	uint32_t lastX = 0, lastY = 0;
	uint32_t polls = 0, posts = 0, moveLogs = 0;
	uint32_t errCount = 0;

	IOLog("OP2I2C: touch poll thread started (20ms)\n");
	for (;;) {
		IOSleep(20);
		polls++;

		uint8_t w[2];
		if (synaRead(kSynaF01Data, w, 2) != 0) {
			if (++errCount < 6) {
				IOLog("OP2I2C: poll: F01 word read failed\n");
			}
			continue;
		}
		uint8_t status = w[0], irq = w[1];
		if (status != 0) {
			/* Soft reset (F01 cmd0=0x01), 170ms, re-init */
			if (errCount++ < 6) {
				IOLog("OP2I2C: poll: status=%02x, soft reset\n", status);
			}
			synaWrite(kSynaF01Cmd, 0x01);
			IOSleep(170);
			synaInit();
			continue;
		}
		if ((polls % 1500) == 0) {
			uint32_t attn = rd(fTlmm, OP2_TLMM_INOUT(96)) & 1;
			IOLog("OP2I2C: poll heartbeat polls=%u posts=%u ATTN=%u status=%02x irq=%02x\n",
			    polls, posts, attn, status, irq);
		}
		/* Read fingers on F12 interrupt, or while down (catches UP). */
		if ((irq & kSynaF12Irq) == 0 && !prevDown) {
			continue;
		}
		uint8_t f[8];
		if (synaRead(kSynaF12Data, f, sizeof(f)) != 0) {
			if (++errCount < 6) {
				IOLog("OP2I2C: poll: F12 read failed\n");
			}
			continue;
		}
		uint32_t fs = f[0] & 0x03;
		bool down = (fs == 1 || fs == 2);
		uint32_t x = (uint32_t)(((f[2] & 0x0F) << 8) | f[1]);
		uint32_t y = (uint32_t)(((f[4] & 0x0F) << 8) | f[3]);
		if (x > 1079) {
			x = 1079;
		}
		if (y > 1919) {
			y = 1919;
		}

		uint32_t dx = (x > lastX) ? (x - lastX) : (lastX - x);
		uint32_t dy = (y > lastY) ? (y - lastY) : (lastY - y);
		bool moved = (dx + dy) > 8;
		if (down != prevDown || (down && moved)) {
			/* Device zeroes coords on lift; report UP at last point */
			uint32_t px = down ? x : lastX;
			uint32_t py = down ? y : lastY;
			uint32_t xfp = (px * 65536U) / 1080U;
			uint32_t yfp = (py * 65536U) / 1920U;
			op2_digitizer_post(down ? 1 : 0, xfp, yfp, f[5]);
			if (down != prevDown && py > 1820 && px > 360 && px < 720) {
				op2_home_button_post(down ? 1 : 0);
				IOLog("OP2I2C: capacitive Home button %s (px=%u py=%u)\n",
				    down ? "DOWN" : "UP  ", px, py);
			}
			posts++;
			if (down != prevDown || (moveLogs++ % 50) == 0) {
				IOLog("OP2I2C: finger %s x=%u y=%u z=%u (polls=%u posts=%u)\n",
				    down ? "DOWN" : "UP  ", px, py, (unsigned)f[5],
				    polls, posts);
			}
			prevDown = down;
			if (down) {
				lastX = x;
				lastY = y;
			}
		}
	}
}

void
OP2I2C::synaProbe(void)
{
	/*
	 * S3320 register map (from synaptics_driver_s3320.c
	 * synaptics_read_register_map + live /dev/rmi1 reads in TWRP):
	 * the PDT is shifted by one from the RMI4 "standard": entries at
	 * 0xDD (F12), 0xE3 (F01), 0xE9 (F34). A 0xEE walk finds fn=00.
	 * F12: q=0x48 c=0x00 ctrl=0x14 data=0x06
	 * F01: q=0x2B c=0x26 ctrl=0x0E data=0x04
	 */
	uint8_t f01Query = 0x2B, f01Data = 0x04, f01Ctrl = 0x0E;
	uint8_t f12Query = 0x48, f12Data = 0x06, f12Ctrl = 0x14;

	/* Force page 0 (prior OS may leave the page register elsewhere) */
	uint8_t pagesel[2] = { 0xFF, 0x00 };
	int psrc = qupWrRd(OP2_SYNA_ADDR, pagesel, 2, NULL, 0);
	IOLog("OP2I2C: page-select rc=%d\n", psrc);

	/* Verify map: F12 descriptor at 0xDD must end with fn=0x12 */
	uint8_t desc[6];
	if (synaRead(0xDD, desc, sizeof(desc)) != 0) {
		IOLog("OP2I2C: F12 descriptor read failed\n");
		return;
	}
	IOLog("OP2I2C: F12 desc=%02x %02x %02x %02x %02x fn=%02x\n",
	    desc[0], desc[1], desc[2], desc[3], desc[4], desc[5]);
	if (desc[5] != 0x12 || desc[0] != f12Query) {
		IOLog("OP2I2C: F12 descriptor mismatch, aborting\n");
		return;
	}
	(void)f01Ctrl; (void)f12Query;

	/* F01 queries: [0]=mfr(0x01 Synaptics), [2..3]=fw, [11..20]=product id */
	uint8_t q[21];
	if (synaRead(f01Query, q, sizeof(q)) != 0) {
		IOLog("OP2I2C: F01 query read failed\n");
		return;
	}
	IOLog("OP2I2C: F01 mfr=%02x prod=%.10s fw=%02x%02x serial=%02x%02x%02x\n",
	    q[0], (const char *)&q[11], q[2], q[3], q[4], q[5], q[6]);

	/* F01 data0: device status */
	uint8_t st;
	if (synaRead(f01Data, &st, 1) == 0) {
		IOLog("OP2I2C: F01 status=0x%02x (0x00=awake, flashprog bit6=%d)\n",
		    st, (st >> 6) & 1);
	}

	/* F12 (S3320 2D): ctrl+0..3 = max_x/max_y (LE), data = 8B/finger */
	{
		uint8_t c8[14];
		if (synaRead(f12Ctrl, c8, sizeof(c8)) == 0) {
			uint32_t mx = (uint32_t)c8[0] | ((uint32_t)c8[1] << 8);
			uint32_t my = (uint32_t)c8[2] | ((uint32_t)c8[3] << 8);
			IOLog("OP2I2C: F12 max_x=%u max_y=%u\n", mx, my);
		}
		uint8_t d0[8];
		if (synaRead(f12Data, d0, sizeof(d0)) == 0) {
			IOLog("OP2I2C: F12 finger0=%02x x=%u y=%u z=%u\n",
			    d0[0],
			    (unsigned)(((d0[2] & 0x0F) << 8) | d0[1]),
			    (unsigned)(((d0[4] & 0x0F) << 8) | d0[3]),
			    (unsigned)d0[5]);
		}
	}
}

bool
OP2I2C::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}

	fQup = (volatile uint32_t *)ml_io_map(OP2_QUP2_PHYS, OP2_QUP2_SIZE);
	fGcc = (volatile uint32_t *)ml_io_map(OP2_GCC_PHYS, OP2_GCC_SIZE);
	fTlmm = (volatile uint32_t *)ml_io_map(OP2_TLMM_PHYS, OP2_TLMM_SIZE);
	if (fQup == NULL || fGcc == NULL || fTlmm == NULL) {
		IOLog("OP2I2C: mapping failed\n");
		return false;
	}

	/* ATTN gpio96 level before touching anything (1 = idle/powered?) */
	uint32_t attn = rd(fTlmm, OP2_TLMM_INOUT(96));
	IOLog("OP2I2C: TLMM gpio96 ATTN level=%u (INOUT=0x%x)\n",
	    attn & 1, attn);

	/* 1v8 enable gpio74 -> output HIGH (OP2 driver: tpd_hw_pwron).
	 * Powers the ATTN pull-up (and touch IO rail switch). */
	uint32_t c74 = rd(fTlmm, OP2_TLMM_CFG(74));
	uint32_t io74 = rd(fTlmm, OP2_TLMM_INOUT(74));
	wr(fTlmm, OP2_TLMM_CFG(74), c74 | (1U << 9)); /* DIR=output */
	wr(fTlmm, OP2_TLMM_INOUT(74), io74 | (1U << 1)); /* OUT=1 */
	IOSleep(10);
	uint32_t attn2 = rd(fTlmm, OP2_TLMM_INOUT(96)) & 1;
	IOLog("OP2I2C: gpio74 1v8 CFG %08x->%08x IO %08x->%08x; reset31 CFG=%08x IO=%08x; ATTN now=%u\n",
	    c74, rd(fTlmm, OP2_TLMM_CFG(74)), io74, rd(fTlmm, OP2_TLMM_INOUT(74)),
	    rd(fTlmm, OP2_TLMM_CFG(31)), rd(fTlmm, OP2_TLMM_INOUT(31)), attn2);

	/* I2C pins 6/7: keep aboot's proven-good CFG (0x2cc: func3, 8mA).
	 * Do NOT rewrite drive strength: TWRP touch works with this value. */
	uint32_t c6 = rd(fTlmm, OP2_TLMM_CFG(6));
	uint32_t c7 = rd(fTlmm, OP2_TLMM_CFG(7));
	IOLog("OP2I2C: gpio6/7 CFG %08x/%08x (left as-is)\n", c6, c7);

	/* RCG state (expect 0 = XO/19.2MHz); clocks on */
	IOLog("OP2I2C: QUP2 RCG CFG=0x%x CMD=0x%x\n",
	    rd(fGcc, OP2_GCC_BLSP1_QUP2_I2C_CFG_RCGR),
	    rd(fGcc, OP2_GCC_BLSP1_QUP2_I2C_CMD_RCGR));
	if (!gccBranchOn(OP2_GCC_BLSP1_AHB_CBCR)) {
		return false;
	}
	if (!gccBranchOn(OP2_GCC_BLSP1_QUP2_I2C_CBCR)) {
		return false;
	}

	/* QUP sanity: IO_MODE fifo sizes, reset, state */
	uint32_t iom = rd(fQup, kQupIoMode);
	IOLog("OP2I2C: QUP IO_MODE=0x%x STATE=0x%x\n", iom, rd(fQup, kQupState));
	qupReset();
	IOLog("OP2I2C: QUP post-reset STATE=0x%x\n", rd(fQup, kQupState));

	synaProbe();
	synaInit();

	thread_t th = THREAD_NULL;
	kern_return_t kr = kernel_thread_start(OP2I2C::pollThread, this, &th);
	IOLog("OP2I2C: poll thread spawn kr=%d\n", kr);
	if (th != THREAD_NULL) {
		thread_deallocate(th);
	}

	registerService();
	IOLog("OP2I2C: started\n");
	return true;
}

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
