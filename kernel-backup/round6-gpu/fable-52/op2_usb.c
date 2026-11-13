/*
 * OnePlus 2 (MSM8994) USB console: a minimal DWC3 device-mode driver presenting a CDC-ACM serial port to the host.
 * docs/USB-CONSOLE-PLAN.md. Stage 1: the kernel console (everything printf/kprintf writes, the same stream ramoops
 * keeps) streams live over bulk IN; bulk OUT is received into a ring for the stage 2 shell.
 *
 * LK's fastboot leaves the DWC3 (2.50a, PA 0xf9200000) powered, clocked, in device mode with its PHY set up and
 * RUN_STOP clear (round fable-49 probe), so this driver only takes over the core: its own event buffer, a device
 * soft reset, high speed, EP0, enumeration. Register names and command semantics follow Linux drivers/usb/dwc3
 * (core.h, gadget.c, ep0.c). Events are polled by a kernel thread (GEVNTSIZ interrupt masked).
 * Boot-arg op2usb=0 disables it (probe and driver).
 */
#if defined(ARM64_BOARD_CONFIG_MSM8994)
#include <sys/param.h>
#include <sys/systm.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <libkern/OSAtomic.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/proc.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/kernel.h>
#include <miscfs/devfs/devfs.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void *IOMallocContiguous(vm_size_t size, vm_size_t alignment, void *physicalAddress);
extern uint64_t kvtophys(vm_offset_t va);
extern void IOSleep(unsigned milliseconds);
extern void IODelay(unsigned microseconds);
extern void (*op2_usb_putc_hook)(int c);   /* pexpert/arm/pe_serial.c op2_ramoops_td0 */

extern proc_t proc_find(int pid);
extern int proc_rele(proc_t p);
extern pid_t proc_pid(proc_t p);
extern proc_t current_proc(void);
extern struct pgrp *proc_pgrp(proc_t p);
extern void pg_rele(struct pgrp *pgrp);
extern void pgsignal(struct pgrp *pgrp, int signum, int checkctty);
extern void psignal(struct proc *p, int signum);

void op2_usb_start(void);

/* ---- DWC3 registers (core.h) ---- */
#define GCTL            0xc110
#define GEVNTADRLO      0xc400
#define GEVNTADRHI      0xc404
#define GEVNTSIZ        0xc408
#define GEVNTCOUNT      0xc40c
#define DCFG            0xc700
#define DCTL            0xc704
#define DEVTEN          0xc708
#define DSTS            0xc70c
#define DALEPENA        0xc720
#define DEPCMDPAR2(n)   (0xc800 + (n) * 0x10)
#define DEPCMDPAR1(n)   (0xc804 + (n) * 0x10)
#define DEPCMDPAR0(n)   (0xc808 + (n) * 0x10)
#define DEPCMD(n)       (0xc80c + (n) * 0x10)

#define DCTL_RUN_STOP   (1u << 31)
#define DCTL_CSFTRST    (1u << 30)
#define DSTS_DEVCTRLHLT (1u << 22)
#define DCFG_DEVADDR_MASK (0x7fu << 3)
#define DCFG_SPEED_MASK 7u
#define DEVTEN_DISCONN  (1u << 0)
#define DEVTEN_USBRST   (1u << 1)
#define DEVTEN_CONNDONE (1u << 2)

#define CMD_SETEPCONFIG      0x01
#define CMD_SETTRANSFRESOURCE 0x02
#define CMD_SETSTALL         0x04
#define CMD_CLEARSTALL       0x05
#define CMD_STARTTRANSFER    0x06
#define CMD_ENDTRANSFER      0x08
#define CMD_DEPSTARTCFG      0x09
#define CMD_HIPRI_FORCERM    (1u << 11)
#define CMD_CMDACT           (1u << 10)
#define CMD_PARAM(x)         ((uint32_t)(x) << 16)

#define CFG_EP_TYPE(t)       (((t) & 3u) << 1)
#define CFG_MAXP(n)          (((n) & 0x7ffu) << 3)
#define CFG_FIFO(n)          (((n) & 0x1fu) << 17)
#define CFG_ACTION_MODIFY    (2u << 30)
#define CFG_XFER_COMPLETE_EN (1u << 8)
#define CFG_XFER_INPROG_EN   (1u << 9)
#define CFG_XFER_NOTREADY_EN (1u << 10)
#define CFG_BINTERVAL_M1(n)  (((n) & 0xffu) << 16)
#define CFG_EP_NUMBER(n)     (((n) & 0x1fu) << 25)

#define TRB_HWO              (1u << 0)
#define TRB_LST              (1u << 1)
#define TRB_ISP_IMI          (1u << 10)
#define TRB_IOC              (1u << 11)
#define TRBCTL_NORMAL        (1u << 4)
#define TRBCTL_SETUP         (2u << 4)
#define TRBCTL_STATUS2       (3u << 4)
#define TRBCTL_STATUS3       (4u << 4)
#define TRBCTL_DATA          (5u << 4)

#define EV_XFERCOMPLETE      1
#define EV_XFERNOTREADY      3
#define EV_STATUS_CTRL_STATUS 2

/* physical endpoints: 0 EP0 OUT, 1 EP0 IN, 2 EP1 OUT (bulk), 3 EP1 IN (bulk), 5 EP2 IN (interrupt) */
#define EP_BULK_OUT 2
#define EP_BULK_IN  3
#define EP_INTR_IN  5

/* ---- DMA memory: one contiguous 32 KB block ---- */
#define OFF_EVT     0x0000   /* 4 KB event buffer */
#define OFF_TRB0    0x1000   /* EP0 TRB */
#define OFF_SETUP   0x1040   /* 8-byte SETUP packet */
#define OFF_EP0BUF  0x1080   /* EP0 data stage, 512 */
#define OFF_TRBOUT  0x1400
#define OFF_TRBIN   0x1440
#define OFF_IN      0x2000   /* bulk IN, 8 KB */
#define OFF_OUT     0x4000   /* bulk OUT, 512 */
#define MEM_BYTES   0x8000
#define EVT_BYTES   4096

static volatile uint32_t *U;
static uint8_t *mem;
static uint32_t mem_pa;
#define RD(o)    (U[(o) / 4])
#define WR(o, v) (U[(o) / 4] = (v))
#define PA(off)  (mem_pa + (off))

static inline void
civac(uint32_t off, uint32_t len)
{
	uintptr_t a = ((uintptr_t)mem + off) & ~(uintptr_t)63, end = ((uintptr_t)mem + off + len + 63) & ~(uintptr_t)63;
	__asm__ volatile("dsb sy" ::: "memory");
	for (; a < end; a += 64)
		__asm__ volatile("dc civac, %0" : : "r"(a) : "memory");
	__asm__ volatile("dsb sy" ::: "memory");
}

/* ---- the console ring: every console byte, lock-free (printf can run anywhere) ---- */
#define RING 65536u
static char ring_buf[RING];
static volatile uint32_t ring_head, ring_tail;
static volatile int usb_configured;

static int wchan_in, wchan_out;   /* sleep/wakeup channels: lines to read, room to write */
static volatile int shell_open;   /* /dev/op2usb open: the kernel log pauses, the terminal belongs to the shell */

static void
ring_put(int c)
{
	uint32_t h = (uint32_t)OSIncrementAtomic((volatile SInt32 *)&ring_head);
	ring_buf[h & (RING - 1)] = (char)c;
}

static void
op2_usb_putc(int c)   /* the console hook */
{
	if (!shell_open)
		ring_put(c);
}

static void
ring_puts(const char *s)
{
	while (*s)
		ring_put(*s++);
}

/* host -> phone: a cooked line discipline (echo, backspace, Enter, ^C, ^D) in front of /dev/op2usb readers */
#define IN_RING 4096u
static char in_ring[IN_RING];     /* complete lines, ready for read() */
static volatile uint32_t in_head, in_tail;
static char line_buf[512];
static uint32_t line_len;
static volatile int in_eof;
static volatile pid_t shell_pid;

static void
in_byte(uint8_t c)
{
	if (!shell_open)
		return;
	if (c == '\r' || c == '\n') {
		ring_puts("\r\n");
		for (uint32_t i = 0; i < line_len; i++)
			in_ring[(in_head++) & (IN_RING - 1)] = line_buf[i];
		in_ring[(in_head++) & (IN_RING - 1)] = '\n';
		line_len = 0;
		wakeup((caddr_t)&wchan_in);
	} else if (c == 0x7f || c == 0x08) {
		if (line_len) {
			line_len--;
			ring_puts("\b \b");
		}
	} else if (c == 0x03) {        /* ^C: send SIGINT to shell process group and drop line */
		line_len = 0;
		ring_puts("^C\r\n");
		in_ring[(in_head++) & (IN_RING - 1)] = '\n';
		wakeup((caddr_t)&wchan_in);
		if (shell_pid > 0) {
			proc_t p = proc_find(shell_pid);
			if (p) {
				struct pgrp *pg = proc_pgrp(p);
				if (pg) {
					pgsignal(pg, 2 /* SIGINT */, 0);
					pg_rele(pg);
				} else {
					psignal(p, 2 /* SIGINT */);
				}
				proc_rele(p);
			}
		}
	} else if (c == 0x04) {        /* ^D on an empty line: end of input */
		if (line_len == 0) {
			in_eof = 1;
			wakeup((caddr_t)&wchan_in);
		}
	} else if (c >= 0x20 || c == '\t') {
		if (line_len < sizeof(line_buf) - 1) {
			line_buf[line_len++] = (char)c;
			ring_put(c);
		}
	}
}

/* ---- endpoint commands ---- */
static int
depcmd(int ep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
	WR(DEPCMDPAR2(ep), p2);
	WR(DEPCMDPAR1(ep), p1);
	WR(DEPCMDPAR0(ep), p0);
	WR(DEPCMD(ep), cmd | CMD_CMDACT);
	for (int i = 0; i < 10000; i++) {
		uint32_t v = RD(DEPCMD(ep));
		if (!(v & CMD_CMDACT))
			return ((v >> 15) & 1) ? -1 : (int)((v >> 16) & 0x7f);   /* DEPCMD_STATUS bit 15; else the rsc idx */
		IODelay(1);
	}
	printf("OP2 USB: ep %d cmd 0x%x timed out\n", ep, cmd);
	return -100;
}

static void
trb(uint32_t off, uint32_t buf_pa, uint32_t len, uint32_t ctrl)
{
	volatile uint32_t *t = (volatile uint32_t *)(void *)(mem + off);
	t[0] = buf_pa;
	t[1] = 0;
	t[2] = len;
	t[3] = ctrl;
	civac(off, 16);
}

static int ep_rsc[6];
static int
start_xfer(int ep, uint32_t trb_off)
{
	int r = depcmd(ep, CMD_STARTTRANSFER, 0, PA(trb_off), 0);
	if (r >= 0)
		ep_rsc[ep] = r;
	return r;
}

/* ---- descriptors: CDC-ACM, Linux gadget serial ids (macOS binds by class) ---- */
static const uint8_t dev_desc[18] = {
	18, 1, 0x00, 0x02, 0x02, 0x00, 0x00, 64, 0x25, 0x05, 0xa7, 0xa4, 0x00, 0x01, 1, 2, 3, 1,
};
static const uint8_t cfg_desc[67] = {
	9, 2, 67, 0, 2, 1, 0, 0x80, 250,
	9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,                 /* comm interface, 1 ep */
	5, 0x24, 0x00, 0x10, 0x01,                          /* CDC header 1.10 */
	5, 0x24, 0x01, 0x00, 1,                             /* call management: data on interface 1 */
	4, 0x24, 0x02, 0x02,                                /* ACM: line coding + control line state */
	5, 0x24, 0x06, 0, 1,                                /* union: master 0, slave 1 */
	7, 5, 0x82, 0x03, 16, 0, 9,                         /* EP2 IN interrupt, 16 B, 2^(9-1) microframes */
	9, 4, 1, 0, 2, 0x0a, 0x00, 0x00, 0,                 /* data interface, 2 eps */
	7, 5, 0x01, 0x02, 0x00, 0x02, 0,                    /* EP1 OUT bulk 512 */
	7, 5, 0x81, 0x02, 0x00, 0x02, 0,                    /* EP1 IN bulk 512 */
};
static uint8_t line_coding[7] = { 0x00, 0xc2, 0x01, 0x00, 0, 0, 8 };   /* 115200 8N1 */

static int
string_desc(int idx, uint8_t *out)
{
	static const char *s[] = { NULL, "OnePlus 2 XNU", "OP2 USB console", "0001" };
	if (idx == 0) {
		out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04;
		return 4;
	}
	if (idx < 1 || idx > 3)
		return -1;
	int n = 2;
	for (const char *p = s[idx]; *p; p++) {
		out[n++] = (uint8_t)*p;
		out[n++] = 0;
	}
	out[0] = (uint8_t)n;
	out[1] = 3;
	return n;
}

/* ---- EP0 control state machine (ep0.c) ---- */
enum { EP0_SETUP, EP0_DATA, EP0_WAIT_STATUS, EP0_STATUS };
static int ep0_state, ep0_three_stage, ep0_status_ep, ep0_data_out_req;
static uint32_t ep0_maxp = 512;
static uint32_t ev_logged;

static void
ep0_setup_start(void)
{
	trb(OFF_TRB0, PA(OFF_SETUP), 8, TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC | TRBCTL_SETUP);
	int r = start_xfer(0, OFF_TRB0);
	if (r < 0)
		printf("OP2 USB: setup start failed %d\n", r);
	ep0_state = EP0_SETUP;
}

static void
ep0_stall(void)
{
	depcmd(0, CMD_SETSTALL, 0, 0, 0);
	ep0_setup_start();
}

static void
ep_enable(int ep, int type, uint32_t maxp, int interval)
{
	uint32_t p0 = CFG_EP_TYPE(type) | CFG_MAXP(maxp) | ((ep & 1) ? CFG_FIFO(ep >> 1) : 0);
	uint32_t p1 = CFG_XFER_COMPLETE_EN | CFG_XFER_NOTREADY_EN | CFG_XFER_INPROG_EN | CFG_EP_NUMBER(ep) |
	    (interval ? CFG_BINTERVAL_M1(interval - 1) : 0);
	int a = depcmd(ep, CMD_SETEPCONFIG, p0, p1, 0);
	int b = depcmd(ep, CMD_SETTRANSFRESOURCE, 1, 0, 0);
	WR(DALEPENA, RD(DALEPENA) | (1u << ep));
	if (a < 0 || b < 0)
		printf("OP2 USB: ep %d enable: %d %d\n", ep, a, b);
}

static volatile int in_busy, out_armed;

static void
out_arm(void)
{
	trb(OFF_TRBOUT, PA(OFF_OUT), 512, TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC | TRBCTL_NORMAL);
	civac(OFF_OUT, 512);
	out_armed = start_xfer(EP_BULK_OUT, OFF_TRBOUT) >= 0;
}

static void
set_configuration(int cfg)
{
	if (cfg == 0) {
		usb_configured = 0;
		return;
	}
	depcmd(0, CMD_DEPSTARTCFG | CMD_PARAM(2), 0, 0, 0);
	ep_enable(EP_BULK_OUT, 2, 512, 0);
	ep_enable(EP_BULK_IN, 2, 512, 0);
	ep_enable(EP_INTR_IN, 3, 16, 9);
	in_busy = 0;
	out_arm();
	usb_configured = 1;
	printf("OP2 USB: configured: CDC-ACM console up\n");
}

/* reply to an IN request: data stage on EP0 IN */
static void
ep0_reply(const void *data, uint32_t len, uint32_t wlength)
{
	if (len > wlength)
		len = wlength;
	if (len > 512)
		len = 512;
	memcpy(mem + OFF_EP0BUF, data, len);
	civac(OFF_EP0BUF, 512);
	trb(OFF_TRB0, PA(OFF_EP0BUF), len, TRB_HWO | TRB_LST | TRB_IOC | TRBCTL_DATA);
	start_xfer(1, OFF_TRB0);
	ep0_state = EP0_DATA;
	ep0_three_stage = 1;
	ep0_status_ep = 0;   /* status OUT */
}

static void
ep0_handle_setup(void)
{
	civac(OFF_SETUP, 8);
	const uint8_t *s = mem + OFF_SETUP;
	uint8_t type = s[0], req = s[1];
	uint16_t value = (uint16_t)(s[2] | s[3] << 8), wlength = (uint16_t)(s[6] | s[7] << 8);
	if (ev_logged < 40) {
		ev_logged++;
		printf("OP2 USB: setup %02x %02x val %04x len %u\n", type, req, value, wlength);
	}
	ep0_three_stage = 0;
	ep0_status_ep = 1;   /* no data stage: status IN */
	ep0_data_out_req = 0;
	uint8_t buf[64];
	switch ((type << 8) | req) {
	case 0x8006: {   /* GET_DESCRIPTOR */
		int dt = value >> 8, idx = value & 0xff, n;
		if (dt == 1) {
			ep0_reply(dev_desc, sizeof(dev_desc), wlength);
		} else if (dt == 2) {
			ep0_reply(cfg_desc, sizeof(cfg_desc), wlength);
		} else if (dt == 3 && (n = string_desc(idx, buf)) > 0) {
			ep0_reply(buf, (uint32_t)n, wlength);
		} else {
			ep0_stall();   /* device qualifier, BOS, other: we are a plain high-speed 2.0 device */
		}
		return;
	}
	case 0x0005:     /* SET_ADDRESS: DCFG now, the status stage completes it (ep0.c) */
		WR(DCFG, (RD(DCFG) & ~DCFG_DEVADDR_MASK) | ((uint32_t)(value & 0x7f) << 3));
		break;
	case 0x0009:     /* SET_CONFIGURATION */
		set_configuration(value & 0xff);
		break;
	case 0x8008:     /* GET_CONFIGURATION */
		buf[0] = (uint8_t)usb_configured;
		ep0_reply(buf, 1, wlength);
		return;
	case 0x8000: case 0x8100: case 0x8200:   /* GET_STATUS */
		buf[0] = 0; buf[1] = 0;
		ep0_reply(buf, 2, wlength);
		return;
	case 0xa121:     /* GET_LINE_CODING */
		ep0_reply(line_coding, 7, wlength);
		return;
	case 0x2120:     /* SET_LINE_CODING: 7 bytes OUT */
		trb(OFF_TRB0, PA(OFF_EP0BUF), 64, TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC | TRBCTL_DATA);
		start_xfer(0, OFF_TRB0);
		ep0_state = EP0_DATA;
		ep0_three_stage = 1;
		ep0_status_ep = 1;   /* status IN */
		ep0_data_out_req = 0x20;
		return;
	case 0x2122:     /* SET_CONTROL_LINE_STATE (DTR/RTS) */
	case 0x0001: case 0x0003: case 0x0101: case 0x0103: case 0x000b: case 0x010b:   /* features, SET_INTERFACE */
	case 0x0201: case 0x0203:
		break;
	default:
		ep0_stall();
		return;
	}
	ep0_state = EP0_WAIT_STATUS;
}

static void
ep0_event(int ep, int evt, int status)
{
	if (evt == EV_XFERCOMPLETE) {
		if (ep0_state == EP0_SETUP && ep == 0) {
			ep0_handle_setup();
		} else if (ep0_state == EP0_DATA) {
			if (ep0_data_out_req == 0x20) {
				civac(OFF_EP0BUF, 64);
				memcpy(line_coding, mem + OFF_EP0BUF, 7);
			}
			ep0_state = EP0_WAIT_STATUS;
		} else if (ep0_state == EP0_STATUS) {
			ep0_setup_start();
		}
	} else if (evt == EV_XFERNOTREADY && status == EV_STATUS_CTRL_STATUS && ep0_state == EP0_WAIT_STATUS &&
	    ep == ep0_status_ep) {
		trb(OFF_TRB0, PA(OFF_EP0BUF), 0, TRB_HWO | TRB_LST | TRB_IOC |
		    (ep0_three_stage ? TRBCTL_STATUS3 : TRBCTL_STATUS2));
		start_xfer(ep, OFF_TRB0);
		ep0_state = EP0_STATUS;
	}
}

static void
ep0_configure(uint32_t maxp, bool modify)
{
	for (int ep = 0; ep < 2; ep++) {
		uint32_t p0 = CFG_EP_TYPE(0) | CFG_MAXP(maxp) | (modify ? CFG_ACTION_MODIFY : 0);
		uint32_t p1 = CFG_XFER_COMPLETE_EN | CFG_XFER_NOTREADY_EN | CFG_EP_NUMBER(ep);
		depcmd(ep, CMD_SETEPCONFIG, p0, p1, 0);
		if (!modify)
			depcmd(ep, CMD_SETTRANSFRESOURCE, 1, 0, 0);
	}
	ep0_maxp = maxp;
}

static void
end_xfer(int ep)
{
	depcmd(ep, CMD_ENDTRANSFER | CMD_HIPRI_FORCERM | CMD_PARAM(ep_rsc[ep]), 0, 0, 0);
}

static void
device_event(int evt)
{
	if (ev_logged < 40) {
		ev_logged++;
		printf("OP2 USB: device event %d (DSTS %08x)\n", evt, RD(DSTS));
	}
	switch (evt) {
	case 0:   /* disconnect */
		usb_configured = 0;
		break;
	case 1:   /* USB reset */
		if (usb_configured || in_busy || out_armed) {
			end_xfer(EP_BULK_IN);
			end_xfer(EP_BULK_OUT);
		}
		usb_configured = 0;
		in_busy = 0;
		out_armed = 0;
		WR(DCFG, RD(DCFG) & ~DCFG_DEVADDR_MASK);
		break;
	case 2: { /* connect done: the speed is known, EP0's max packet with it */
		uint32_t spd = RD(DSTS) & 7;
		ep0_configure(spd == 4 ? 512 : 64, true);
		break;
	}
	default:
		break;
	}
}

static void
ep_event(int ep, int evt, int status)
{
	if (ep <= 1) {
		ep0_event(ep, evt, status);
		return;
	}
	if (evt != EV_XFERCOMPLETE)
		return;
	if (ep == EP_BULK_IN) {
		in_busy = 0;
		wakeup((caddr_t)&wchan_out);
	} else if (ep == EP_BULK_OUT) {
		civac(OFF_TRBOUT, 16);
		uint32_t left = ((volatile uint32_t *)(void *)(mem + OFF_TRBOUT))[2] & 0xffffff;
		uint32_t got = 512 - left;
		civac(OFF_OUT, 512);
		for (uint32_t i = 0; i < got; i++)
			in_byte(mem[OFF_OUT + i]);
		out_arm();
	}
}

/* drain the console ring into bulk IN when the endpoint is free */
static void
pump_in(void)
{
	if (!usb_configured || in_busy)
		return;
	uint32_t head = ring_head, tail = ring_tail;
	if (head == tail)
		return;
	if (head - tail > RING)
		tail = head - RING + 1024;   /* overrun: skip to the newest bytes */
	uint32_t n = head - tail;
	if (n > 8192)
		n = 8192;
	for (uint32_t i = 0; i < n; i++)
		mem[OFF_IN + i] = (uint8_t)ring_buf[(tail + i) & (RING - 1)];
	civac(OFF_IN, n);
	trb(OFF_TRBIN, PA(OFF_IN), n, TRB_HWO | TRB_LST | TRB_IOC | TRBCTL_NORMAL);
	if (start_xfer(EP_BULK_IN, OFF_TRBIN) >= 0) {
		in_busy = 1;
		ring_tail = tail + n;
	}
}

static uint32_t evt_pos;

static void
poll_events(void)
{
	uint32_t cnt = RD(GEVNTCOUNT) & 0xffff;
	while (cnt >= 4) {
		civac(OFF_EVT + evt_pos, 4);
		uint32_t ev = *(volatile uint32_t *)(void *)(mem + OFF_EVT + evt_pos);
		evt_pos = (evt_pos + 4) % EVT_BYTES;
		cnt -= 4;
		WR(GEVNTCOUNT, 4);
		if (ev & 1) {
			if (((ev >> 1) & 0x7f) == 0)
				device_event((ev >> 8) & 0xf);
		} else {
			ep_event((ev >> 1) & 0x1f, (ev >> 6) & 0xf, (ev >> 12) & 0xf);
		}
	}
}

static int
wait_bit(uint32_t reg, uint32_t bit, bool set, int ms)
{
	for (int i = 0; i < ms * 10; i++) {
		if (((RD(reg) & bit) != 0) == set)
			return 0;
		IODelay(100);
	}
	return -1;
}

/* ---- /dev/op2usb: the shell's stdin/stdout ---- */
static int
op2usb_open(__unused dev_t dev, __unused int flags, __unused int devtype, struct proc *p)
{
	line_len = 0;
	in_tail = in_head;
	in_eof = 0;
	shell_open++;
	if (p)
		shell_pid = proc_pid(p);
	return 0;
}

static int
op2usb_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	if (shell_open > 0)
		shell_open--;
	ring_puts("\r\n[op2usb: shell closed, kernel log resumes]\r\n");
	return 0;
}

static int
op2usb_read(__unused dev_t dev, struct uio *uio, int ioflag)
{
	proc_t cp = current_proc();
	if (cp)
		shell_pid = proc_pid(cp);
	while (in_tail == in_head) {
		if (in_eof) {
			in_eof = 0;
			return 0;
		}
		if (ioflag & IO_NDELAY)
			return EWOULDBLOCK;
		int e = tsleep((caddr_t)&wchan_in, PZERO | PCATCH, "op2usb", hz / 10);
		if (e && e != EWOULDBLOCK)
			return e;
	}
	char buf[256];
	while (in_tail != in_head && uio_resid(uio) > 0) {
		uint32_t n = 0;
		while (in_tail != in_head && n < sizeof(buf) && (user_ssize_t)n < uio_resid(uio)) {
			buf[n++] = in_ring[(in_tail++) & (IN_RING - 1)];
			if (buf[n - 1] == '\n')
				break;
		}
		int e = uiomove(buf, (int)n, uio);
		if (e)
			return e;
		if (buf[n - 1] == '\n')
			break;   /* a line per read, like a tty in canonical mode */
	}
	return 0;
}

static int
op2usb_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	char buf[256];
	while (uio_resid(uio) > 0) {
		int n = (int)MIN((user_ssize_t)sizeof(buf), uio_resid(uio));
		int e = uiomove(buf, n, uio);
		if (e)
			return e;
		for (int i = 0; i < n; i++) {
			while (ring_head - ring_tail > RING - 1024 && usb_configured)
				tsleep((caddr_t)&wchan_out, PZERO, "op2usbw", 1);   /* let bulk IN drain */
			if (buf[i] == '\n')
				ring_put('\r');
			ring_put(buf[i]);
		}
	}
	return 0;
}

static int
op2usb_ioctl(__unused dev_t dev, __unused u_long cmd, __unused caddr_t data, __unused int fflag,
    __unused struct proc *p)
{
	return ENOTTY;
}

static struct cdevsw op2usb_cdevsw = {
	.d_open = op2usb_open,
	.d_close = op2usb_close,
	.d_read = op2usb_read,
	.d_write = op2usb_write,
	.d_ioctl = op2usb_ioctl,
	.d_stop = eno_stop,
	.d_reset = eno_reset,
	.d_ttys = NULL,
	.d_select = (select_fcn_t *)seltrue,
	.d_mmap = eno_mmap,
	.d_strategy = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type = 0,
};

__attribute__((noreturn)) static void
op2_usb_thread(__unused void *param, __unused wait_result_t wr)
{
	/* 1. stop, device soft reset */
	WR(DCTL, RD(DCTL) & ~DCTL_RUN_STOP);
	int halted = wait_bit(DSTS, DSTS_DEVCTRLHLT, true, 100);
	WR(DCTL, RD(DCTL) | DCTL_CSFTRST);
	int reset = wait_bit(DCTL, DCTL_CSFTRST, false, 500);
	/* 2. our event buffer, interrupt masked (polled) */
	memset(mem, 0, MEM_BYTES);
	civac(0, MEM_BYTES);
	WR(GEVNTADRLO, PA(OFF_EVT));
	WR(GEVNTADRHI, 0);
	WR(GEVNTSIZ, EVT_BYTES | (1u << 31));
	WR(GEVNTCOUNT, RD(GEVNTCOUNT) & 0xffff);
	evt_pos = 0;
	/* 3. high speed only, address 0, the device events */
	WR(DCFG, (RD(DCFG) & ~(DCFG_SPEED_MASK | DCFG_DEVADDR_MASK)) | 0);
	WR(DEVTEN, DEVTEN_DISCONN | DEVTEN_USBRST | DEVTEN_CONNDONE);
	/* 4. EP0 */
	depcmd(0, CMD_DEPSTARTCFG | CMD_PARAM(0), 0, 0, 0);
	ep0_configure(512, false);
	WR(DALEPENA, 3);
	ep0_setup_start();
	/* 5. run */
	WR(DCTL, (RD(DCTL) & ~DCTL_CSFTRST) | DCTL_RUN_STOP);
	int running = wait_bit(DSTS, DSTS_DEVCTRLHLT, false, 100);
	printf("OP2 USB: takeover: halt %d reset %d run %d; DCFG %08x DCTL %08x DSTS %08x GEVNTSIZ %08x mem PA %08x\n",
	    halted, reset, running, RD(DCFG), RD(DCTL), RD(DSTS), RD(GEVNTSIZ), mem_pa);
	op2_usb_putc_hook = op2_usb_putc;   /* the console from here on also goes to the ring */
	bool node_made = false;
	for (uint32_t n = 0;; n++) {
		poll_events();
		pump_in();
		/* /dev/op2usb once devfs exists: this thread starts long before it (round fable-51: devfs_make_node from
		 * here at boot panicked "Invalid mutex"); the root filesystem being mounted means devfs is up */
		extern struct vnode *rootvnode;
		if (!node_made && rootvnode != NULL && (n % 100) == 0) {
			node_made = true;
			int cmajor = cdevsw_add(-1, &op2usb_cdevsw);
			if (cmajor >= 0)
				devfs_make_node(makedev(cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600, "op2usb");
			printf("OP2 USB: /dev/op2usb major %d\n", cmajor);
		}
		if (n == 30000)
			printf("OP2 USB: 30 s: configured %d, line %u baud, %u bytes in\n", usb_configured,
			    line_coding[0] | line_coding[1] << 8 | line_coding[2] << 16, (uint32_t)in_head);
		IOSleep(1);
	}
}

void
op2_usb_start(void)
{
	uint32_t en = 1;
	PE_parse_boot_argn("op2usb", &en, sizeof(en));
	if (!en)
		return;
	U = (volatile uint32_t *)ml_io_map(0xf9200000, 0x100000);
	mem = (uint8_t *)IOMallocContiguous(MEM_BYTES, 4096, NULL);
	if (!U || !mem) {
		printf("OP2 USB: no map / memory\n");
		return;
	}
	uint64_t pa = kvtophys((vm_offset_t)mem), pa_end = kvtophys((vm_offset_t)mem + MEM_BYTES - 1);
	if (pa == 0 || pa_end >= (1ULL << 32) || pa_end - pa != MEM_BYTES - 1) {
		printf("OP2 USB: DMA memory at 0x%llx not 32-bit contiguous\n", (unsigned long long)pa);
		return;
	}
	mem_pa = (uint32_t)pa;
	thread_t th;
	if (kernel_thread_start(op2_usb_thread, NULL, &th) == KERN_SUCCESS)
		thread_deallocate(th);
}
#endif /* ARM64_BOARD_CONFIG_MSM8994 */
