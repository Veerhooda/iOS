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
#include <sys/ioctl.h>
#include <sys/ttycom.h>
#include <sys/termios.h>
#include <sys/proc_internal.h>
#define TTYDEFCHARS
#include <sys/ttydefaults.h>
#include <kern/locks.h>
#include <sys/kpi_mbuf.h>
#include <sys/kpi_socket.h>
#include <sys/sockio.h>
#include <net/if.h>
#include <net/if_types.h>
#include <net/ethernet.h>
#include <net/if_ether.h>
#include <net/ether_if_module.h>
#include <net/kpi_interface.h>
#include <net/kpi_protocol.h>
#include <netinet/in.h>
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

/* physical endpoints: 0 EP0 OUT, 1 EP0 IN, 2 EP1 OUT (bulk), 3 EP1 IN (bulk), 5 EP2 IN (interrupt),
 * 4 ECM OUT (bulk), 7 ECM IN (bulk), 9 ECM INT (interrupt). OUT addr N = phys 2N, IN addr N = phys 2N+1. */
#define EP_BULK_OUT 2
#define EP_BULK_IN  3
#define EP_INTR_IN  5
#define EP_ECM_OUT 4   /* addr 0x02 bulk OUT */
#define EP_ECM_IN  7   /* addr 0x83 bulk IN */
#define EP_ECM_INT 9   /* addr 0x84 interrupt IN */

/* ---- DMA memory: one contiguous 32 KB block ---- */
#define OFF_EVT     0x0000   /* 4 KB event buffer */
#define OFF_TRB0    0x1000   /* EP0 TRB */
#define OFF_SETUP   0x1040   /* 8-byte SETUP packet */
#define OFF_EP0BUF  0x1080   /* EP0 data stage, 512 */
#define OFF_TRBOUT  0x1400
#define OFF_TRBIN   0x1440
#define OFF_IN      0x2000   /* bulk IN, 8 KB */
#define OFF_OUT     0x4000   /* bulk OUT, 512 */
#define OFF_TRBECM  0x4200   /* 3 ECM TRBs: OUT, IN, INT (16 B each) */
#define OFF_ECM_NFY 0x4240   /* ECM notification, 16 B */
#define OFF_ECM_RX  0x4300   /* one received frame, 2 KB */
#define OFF_ECM_TX  0x4b00   /* one frame to send, 2 KB */
#define ECM_FRAME_MAX 0x800
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

/* minimal tty face: dash's job control needs isatty (FIODTYPE == D_TTY, via .d_type below: iOS 12's
 * isatty never reaches TIOCGETA when FIODTYPE succeeds) and tcgetpgrp/tcsetpgrp (TIOCGPGRP/TIOCSPGRP)
 * to succeed; winsize for COLUMNS/LINES. The line discipline itself stays fixed (see in_byte): TIOCSETA is
 * stored so GET reads back what was SET. No controlling-terminal binding (no TIOCSCTTY): the checks below
 * are pgfind + same-session, the isctty part of tty.c's TIOCSPGRP skipped by design. */
static pid_t tty_fg;                 /* foreground pgrp; 0 = unset: answer the caller's own group */
static struct termios tty_termios;
static int tty_termios_set;
static struct winsize tty_winsize = { 24, 80, 0, 0 };
static int tty_fg_logged;            /* one live fg line per open */

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
	} else if (c == 0x03) {        /* ^C: SIGINT the foreground group (else the shell's) and drop line */
		line_len = 0;
		ring_puts("^C\r\n");
		in_ring[(in_head++) & (IN_RING - 1)] = '\n';
		wakeup((caddr_t)&wchan_in);
		int done = 0;
		if (tty_fg > 0) {   /* job control on: dash moves this per job, so ^C hits the foreground child */
			struct pgrp *fg = pgfind(tty_fg);
			if (fg != PGRP_NULL) {
				pgsignal(fg, 2 /* SIGINT */, 0);
				pg_rele(fg);
				done = 1;
			} else {
				tty_fg = 0;   /* stale group: drop to the shell path */
			}
		}
		if (!done && shell_pid > 0) {
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

static int ep_rsc[10];   /* transfer resource idx, by physical ep (ECM uses 4, 7, 9) */
static int ep_started[10];
static unsigned start_fail;   /* STARTTRANSFER failures, all endpoints (in the periodic line) */

static void
start_failed(const char *site, int r)
{
	start_fail++;
	if (start_fail < 8)
		printf("OP2 USB: start failed: %s %d\n", site, r);
}

static int
start_xfer(int ep, uint32_t trb_off)
{
	int r = depcmd(ep, CMD_STARTTRANSFER, 0, PA(trb_off), 0);
	if (r >= 0) {
		ep_rsc[ep] = r;
		ep_started[ep] = 1;
	}
	return r;
}

/* ---- descriptors: composite CDC-ACM (console) + CDC-ECM (network), Linux gadget serial ids ---- */
static const uint8_t dev_desc[18] = {
	18, 1, 0x00, 0x02, 0xef, 0x02, 0x01, 64, 0x25, 0x05, 0xa7, 0xa4, 0x00, 0x01, 1, 2, 3, 1,
};
static const uint8_t cfg_desc[154] = {
	9, 2, 154, 0, 4, 1, 0, 0x80, 250,                /* config: 4 interfaces, 500 mA */
	8, 11, 0, 2, 0x02, 0x02, 0x01, 0,               /* IAD: ACM on interfaces 0-1 */
	9, 4, 0, 0, 1, 0x02, 0x02, 0x01, 0,             /* ACM comm interface, 1 ep */
	5, 0x24, 0x00, 0x10, 0x01,                      /* CDC header 1.10 */
	5, 0x24, 0x01, 0x00, 1,                         /* call management: data on interface 1 */
	4, 0x24, 0x02, 0x02,                            /* ACM: line coding + control line state */
	5, 0x24, 0x06, 0, 1,                            /* union: master 0, slave 1 */
	7, 5, 0x82, 0x03, 16, 0, 9,                     /* EP2 IN interrupt, 16 B, 2^(9-1) microframes */
	9, 4, 1, 0, 2, 0x0a, 0x00, 0x00, 0,             /* ACM data interface, 2 eps */
	7, 5, 0x01, 0x02, 0x00, 0x02, 0,                /* EP1 OUT bulk 512 */
	7, 5, 0x81, 0x02, 0x00, 0x02, 0,                /* EP1 IN bulk 512 */
	8, 11, 2, 2, 0x02, 0x06, 0x00, 0,               /* IAD: ECM on interfaces 2-3 */
	9, 4, 2, 0, 1, 0x02, 0x06, 0x00, 0,             /* ECM comm interface, 1 ep */
	5, 0x24, 0x00, 0x10, 0x01,                      /* CDC header 1.10 */
	5, 0x24, 0x06, 2, 3,                            /* union: master 2, slave 3 */
	13, 0x24, 0x0f, 4, 0, 0, 0, 0, 0xea, 0x05, 0, 0, 0, /* ethernet: iMAC 4, max segment 1514 */
	7, 5, 0x84, 0x03, 16, 0, 9,                     /* EP4 IN interrupt, 16 B */
	9, 4, 3, 0, 0, 0x0a, 0x00, 0x00, 0,             /* ECM data alt 0: no eps */
	9, 4, 3, 1, 2, 0x0a, 0x00, 0x00, 0,             /* ECM data alt 1: two bulk eps */
	7, 5, 0x02, 0x02, 0x00, 0x02, 0,                /* EP2 OUT bulk 512 */
	7, 5, 0x83, 0x02, 0x00, 0x02, 0,                /* EP3 IN bulk 512 */
};
static uint8_t line_coding[7] = { 0x00, 0xc2, 0x01, 0x00, 0, 0, 8 };   /* 115200 8N1 */

static int
string_desc(int idx, uint8_t *out)
{
	/* string 4 is the ECM host-side MAC (the phone's own is ..:02, S2) */
	static const char *s[] = { NULL, "OnePlus 2 XNU", "OP2 USB console", "0001", "02005E77EE01" };
	if (idx == 0) {
		out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04;
		return 4;
	}
	if (idx < 1 || idx > 4)
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
	int r = start_xfer(EP_BULK_OUT, OFF_TRBOUT);
	out_armed = r >= 0;
	if (!out_armed)
		start_failed("bulk-out", r);
}

/* ---- ECM: endpoints + link notification (S1); ue0 network interface (S2) ---- */
static int ecm_alt;   /* ECM data interface (3) alternate setting */
static int ecm_desc_err = -1;
static volatile int ecm_notify_busy;

#define ECM_TXQ 32
static ifnet_t ecm_ifp;
static lck_mtx_t *ecm_lock;   /* TX queue and TX endpoint */
static mbuf_t ecm_txq[ECM_TXQ];
static unsigned ecm_txq_head, ecm_txq_count;
static int ecm_tx_busy, ecm_rx_armed;
static uint64_t ecm_rx_frames, ecm_tx_frames;
static unsigned ecm_rx_drops;
static const uint8_t ecm_phone_mac[ETHER_ADDR_LEN] = { 0x02, 0x00, 0x5e, 0x77, 0xee, 0x02 };

static void
ecm_rx_arm(void)
{
	int r;
	if (ecm_rx_armed || !usb_configured)
		return;
	trb(OFF_TRBECM, PA(OFF_ECM_RX), ECM_FRAME_MAX,
	    TRB_HWO | TRB_LST | TRB_ISP_IMI | TRB_IOC | TRBCTL_NORMAL);
	civac(OFF_ECM_RX, ECM_FRAME_MAX);
	r = start_xfer(EP_ECM_OUT, OFF_TRBECM);
	if (r >= 0)
		ecm_rx_armed = 1;
	else
		start_failed("ecm-rx", r);
}

static void
ecm_rx_done(void)
{
	volatile uint32_t *t = (volatile uint32_t *)(void *)(mem + OFF_TRBECM);
	uint32_t len;
	ecm_rx_armed = 0;
	civac(OFF_TRBECM, 16);
	len = ECM_FRAME_MAX - (t[2] & 0xffffff);
	civac(OFF_ECM_RX, ECM_FRAME_MAX);
	if (ecm_ifp != NULL && len >= ETHER_HDR_LEN && len <= ETHER_MAX_LEN) {
		/* single mbuf (maxchunks 1): copyback on a fresh 2-mbuf packet fills only the second
		 * mbuf, leaving the first at length 0, and the header split below would underflow. */
		unsigned int one = 1;
		mbuf_t m = NULL;
		if (mbuf_allocpacket(MBUF_DONTWAIT, len, &one, &m) == 0 &&
		    mbuf_copyback(m, 0, len, (const void *)(mem + OFF_ECM_RX), MBUF_DONTWAIT) == 0 &&
		    mbuf_len(m) >= ETHER_HDR_LEN) {
			struct ifnet_stat_increment_param stats = { 0 };
			void *frame_header = mbuf_data(m);
			mbuf_pkthdr_setrcvif(m, ecm_ifp);
			mbuf_pkthdr_setheader(m, frame_header);
			mbuf_pkthdr_adjustlen(m, -ETHER_HDR_LEN);
			(void)mbuf_setdata(m, (char *)mbuf_data(m) + ETHER_HDR_LEN,
			    mbuf_len(m) - ETHER_HDR_LEN);
			stats.packets_in = 1;
			stats.bytes_in = len;
			ecm_rx_frames++;
			ifnet_input(ecm_ifp, m, &stats);
		} else {
			if (m != NULL) {
				if (ecm_rx_drops < 8)
					printf("OP2 USB: ECM: rx drop len %u mlen %d\n", len,
					    (int)mbuf_len(m));
				mbuf_freem(m);
			}
			ecm_rx_drops++;
		}
	}
	ecm_rx_arm();
}

/* Start the next queued frame if the IN endpoint is idle. ecm_lock held. */
static void
ecm_tx_kick_locked(void)
{
	while (!ecm_tx_busy && ecm_txq_count > 0 && usb_configured) {
		mbuf_t m = ecm_txq[ecm_txq_head];
		size_t len;
		int r;
		ecm_txq_head = (ecm_txq_head + 1) % ECM_TXQ;
		ecm_txq_count--;
		len = mbuf_pkthdr_len(m);
		if (len > ETHER_MAX_LEN - ETHER_CRC_LEN) {
			mbuf_freem(m);
			continue;
		}
		mbuf_copydata(m, 0, len, (void *)(mem + OFF_ECM_TX));
		mbuf_freem(m);
		if ((len % 512) == 0)
			mem[OFF_ECM_TX + len++] = 0;   /* pad instead of a zero-length packet */
		civac(OFF_ECM_TX, (uint32_t)len);
		trb(OFF_TRBECM + 16, PA(OFF_ECM_TX), (uint32_t)len,
		    TRB_HWO | TRB_LST | TRB_IOC | TRBCTL_NORMAL);
		r = start_xfer(EP_ECM_IN, OFF_TRBECM + 16);
		if (r < 0) {
			start_failed("ecm-tx", r);
			return;
		}
		ecm_tx_busy = 1;
		ecm_tx_frames++;
	}
}

static errno_t
ecm_if_output(ifnet_t ifp, mbuf_t m)
{
	lck_mtx_lock(ecm_lock);
	if (ecm_txq_count == ECM_TXQ || !usb_configured) {
		lck_mtx_unlock(ecm_lock);
		mbuf_freem(m);
		ifnet_stat_increment_out(ifp, 0, 0, 1);
		return 0;
	}
	ifnet_stat_increment_out(ifp, 1, mbuf_pkthdr_len(m), 0);
	ecm_txq[(ecm_txq_head + ecm_txq_count) % ECM_TXQ] = m;
	ecm_txq_count++;
	ecm_tx_kick_locked();
	lck_mtx_unlock(ecm_lock);
	return 0;
}

static errno_t
ecm_if_ioctl(ifnet_t ifp, unsigned long cmd, __unused void *data)
{
	switch (cmd) {
	case SIOCSIFADDR:
	case SIOCAIFADDR:
		ifnet_set_flags(ifp, IFF_UP, IFF_UP);
		return 0;
	case SIOCSIFFLAGS:
	case SIOCADDMULTI:
	case SIOCDELMULTI:
		return 0;
	default:
		return EOPNOTSUPP;
	}
}

static void
ecm_if_detach(__unused ifnet_t ifp)
{
}

/* ue0, 192.168.7.2/24 (the Mac side is 192.168.7.1). */
static void
ecm_if_create(void)
{
	struct ifnet_init_eparams init;
	lck_grp_t *grp;
	errno_t err;
	socket_t so = NULL;

	grp = lck_grp_alloc_init("op2usb-ecm", LCK_GRP_ATTR_NULL);
	ecm_lock = lck_mtx_alloc_init(grp, LCK_ATTR_NULL);

	bzero(&init, sizeof(init));
	init.ver = IFNET_INIT_CURRENT_VERSION;
	init.len = sizeof(init);
	init.flags = IFNET_INIT_LEGACY;
	init.uniqueid = "op2-usb-ecm";
	init.uniqueid_len = sizeof("op2-usb-ecm") - 1;
	init.name = "ue";
	init.unit = 0;
	init.family = IFNET_FAMILY_ETHERNET;
	init.type = IFT_ETHER;
	init.output = ecm_if_output;
	init.demux = ether_demux;
	init.add_proto = ether_add_proto;
	init.del_proto = ether_del_proto;
	init.check_multi = ether_check_multi;
	init.framer_extended = ether_frameout_extended;
	init.ioctl = ecm_if_ioctl;
	init.detach = ecm_if_detach;
	init.broadcast_addr = etherbroadcastaddr;
	init.broadcast_len = ETHER_ADDR_LEN;
	if (ifnet_allocate_extended(&init, &ecm_ifp) != 0) {
		printf("OP2 USB: ECM: ifnet_allocate failed\n");
		ecm_ifp = NULL;
		return;
	}
	ifnet_set_addrlen(ecm_ifp, ETHER_ADDR_LEN);
	ifnet_set_hdrlen(ecm_ifp, sizeof(struct ether_header));
	ifnet_set_mtu(ecm_ifp, ETHERMTU);
	ifnet_set_baudrate(ecm_ifp, 480000000);
	ifnet_set_flags(ecm_ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST, 0xffff);
	if (ifnet_attach(ecm_ifp, NULL) != 0) {
		printf("OP2 USB: ECM: ifnet_attach failed\n");
		ecm_ifp = NULL;
		return;
	}
	ifnet_set_lladdr(ecm_ifp, ecm_phone_mac, ETHER_ADDR_LEN);

	err = proto_plumb(PF_INET, ecm_ifp);
	if (err == 0)
		err = sock_socket(PF_INET, SOCK_DGRAM, 0, NULL, NULL, &so);
	if (err == 0) {
		struct ifaliasreq ifra;
		struct sockaddr_in a4;
		bzero(&ifra, sizeof(ifra));
		snprintf(ifra.ifra_name, sizeof(ifra.ifra_name), "ue0");
		bzero(&a4, sizeof(a4));
		a4.sin_len = sizeof(a4);
		a4.sin_family = AF_INET;
		a4.sin_addr.s_addr = htonl(0xc0a80702);   /* 192.168.7.2 */
		memcpy(&ifra.ifra_addr, &a4, sizeof(a4));
		a4.sin_addr.s_addr = htonl(0xffffff00);
		memcpy(&ifra.ifra_mask, &a4, sizeof(a4));
		a4.sin_addr.s_addr = htonl(0xc0a807ff);
		memcpy(&ifra.ifra_broadaddr, &a4, sizeof(a4));
		err = sock_ioctl(so, SIOCAIFADDR, &ifra);
		sock_close(so);
	}
	ifnet_set_flags(ecm_ifp, IFF_UP | IFF_RUNNING, IFF_UP | IFF_RUNNING);
	printf("OP2 USB: ECM: ue0 192.168.7.2/24 %s (err %d)\n", err ? "NOT configured" : "up", err);
}

static void
ecm_net_down(void)   /* drop queued frames on reset/unconfigure; RX re-arms at configure */
{
	if (ecm_lock == NULL)
		return;
	lck_mtx_lock(ecm_lock);
	while (ecm_txq_count > 0) {
		mbuf_freem(ecm_txq[ecm_txq_head]);
		ecm_txq_head = (ecm_txq_head + 1) % ECM_TXQ;
		ecm_txq_count--;
	}
	ecm_tx_busy = 0;
	lck_mtx_unlock(ecm_lock);
	ecm_rx_armed = 0;
}

static void
ecm_notify(void)   /* NETWORK_CONNECTION(connected): the host brings its link up on it */
{
	uint8_t *n;
	if (ecm_notify_busy || !usb_configured)
		return;
	n = mem + OFF_ECM_NFY;
	n[0] = 0xa1; n[1] = 0x00;
	n[2] = 1; n[3] = 0;
	n[4] = 2; n[5] = 0;   /* interface 2 */
	n[6] = 0; n[7] = 0;
	civac(OFF_ECM_NFY, 16);
	trb(OFF_TRBECM + 32, PA(OFF_ECM_NFY), 8, TRB_HWO | TRB_LST | TRB_IOC | TRBCTL_NORMAL);
	int r = start_xfer(EP_ECM_INT, OFF_TRBECM + 32);
	if (r >= 0) {
		ecm_notify_busy = 1;
		printf("OP2 USB: ECM: notified connected\n");
	} else {
		start_failed("ecm-notify", r);
	}
}

/* descriptor self-check: walk the config, count interfaces/IADs/endpoints, endpoint addresses unique.
 * Static-byte mistakes are the main way S1 fails, so this runs before the takeover. One line. */
static void
ecm_desc_check(void)
{
	uint32_t off = 0, ifaces = 0, iads = 0, eps = 0, seen = 0;
	int err = 0;
	if (sizeof(cfg_desc) != 154 || cfg_desc[0] != 9 || cfg_desc[1] != 2 ||
	    cfg_desc[2] != sizeof(cfg_desc) || cfg_desc[3] != 0 || cfg_desc[4] != 4)
		err |= 1;
	while (off < sizeof(cfg_desc)) {
		uint32_t len = cfg_desc[off], type = off + 1 < sizeof(cfg_desc) ? cfg_desc[off + 1] : 0;
		if (len < 2 || off + len > sizeof(cfg_desc)) {
			err |= 2;
			break;
		}
		if (type == 4 && len == 9 && cfg_desc[off + 3] == 0)
			ifaces++;
		else if (type == 11 && len == 8)
			iads++;
		else if (type == 5 && len == 7) {
			uint32_t a = cfg_desc[off + 2];
			uint32_t bit = (a & 0x80) ? (1u << (16 + (a & 0x0f))) : (1u << (a & 0x0f));
			eps++;
			if (a == 0 || a == 0x80 || (a & 0x7f) == 0 || (seen & bit)) {
				err |= 4;
			} else {
				seen |= bit;
			}
		}
		off += len;
	}
	if (ifaces != 4 || iads != 2 || eps != 6)
		err |= 8;
	ecm_desc_err = err;
	printf("OP2 USB: ECM desc check: %s (err 0x%x, if %u iad %u ep %u)\n", err ? "FAIL" : "PASS", err, ifaces,
	    iads, eps);
}

static void
set_configuration(int cfg)
{
	if (cfg == 0) {
		usb_configured = 0;
		ecm_alt = 0;
		ecm_notify_busy = 0;
		ecm_net_down();
		return;
	}
	depcmd(0, CMD_DEPSTARTCFG | CMD_PARAM(2), 0, 0, 0);   /* XferRscIdx 2: all non-EP0 eps, once */
	ep_enable(EP_BULK_OUT, 2, 512, 0);
	ep_enable(EP_BULK_IN, 2, 512, 0);
	ep_enable(EP_INTR_IN, 3, 16, 9);
	ep_enable(EP_ECM_OUT, 2, 512, 0);
	ep_enable(EP_ECM_IN, 2, 512, 0);
	ep_enable(EP_ECM_INT, 3, 16, 9);
	in_busy = 0;
	out_arm();
	usb_configured = 1;
	ecm_rx_arm();   /* ue0 itself is created at mount-root time (below), not here: SET_CONFIGURATION can
	                 * arrive before dlil_init, and ifnet_allocate then panics on its lock */
	printf("OP2 USB: configured: CDC-ACM console + CDC-ECM up\n");
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
	uint16_t value = (uint16_t)(s[2] | s[3] << 8), index = (uint16_t)(s[4] | s[5] << 8),
	    wlength = (uint16_t)(s[6] | s[7] << 8);
	if (ev_logged < 40) {
		ev_logged++;
		printf("OP2 USB: setup %02x %02x val %04x idx %04x len %u\n", type, req, value, index, wlength);
	}
	ep0_three_stage = 0;
	ep0_status_ep = 1;   /* no data stage: status IN */
	ep0_data_out_req = 0;
	/* class OUT with no data stage (SET_ETHERNET_PACKET_FILTER, SET_CONTROL_LINE_STATE): accept */
	if ((type & 0xe0) == 0x20 && wlength == 0 && req != 0x20) {
		ep0_state = EP0_WAIT_STATUS;
		return;
	}
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
	case 0x0001: case 0x0003: case 0x0101: case 0x0103: case 0x000b:   /* features, endpoint SET_INTERFACE */
	case 0x0201: case 0x0203:
		break;
	case 0x010b:     /* SET_INTERFACE (interface recipient) */
		if (index == 3) {
			ecm_alt = value & 0xff;
			printf("OP2 USB: ECM: data alt %d\n", ecm_alt);
			if (ecm_alt == 1 && usb_configured) {
				ecm_notify();
				ecm_rx_arm();
			}
		}
		break;
	case 0x810a:     /* GET_INTERFACE */
		buf[0] = index == 3 ? (uint8_t)ecm_alt : 0;
		ep0_reply(buf, 1, wlength);
		return;
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
	if (!ep_started[ep])
		return;   /* never started: no resource to end (a bogus idx would hit a live transfer) */
	ep_started[ep] = 0;
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
			end_xfer(EP_ECM_IN);
			end_xfer(EP_ECM_OUT);
			end_xfer(EP_ECM_INT);
		}
		usb_configured = 0;
		in_busy = 0;
		out_armed = 0;
		ecm_alt = 0;
		ecm_notify_busy = 0;
		ecm_net_down();
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
	} else if (ep == EP_ECM_INT) {
		ecm_notify_busy = 0;
	} else if (ep == EP_ECM_OUT) {
		ecm_rx_done();
	} else if (ep == EP_ECM_IN) {
		if (ecm_lock != NULL) {
			lck_mtx_lock(ecm_lock);
			ecm_tx_busy = 0;
			ecm_tx_kick_locked();
			lck_mtx_unlock(ecm_lock);
		}
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
	int r = start_xfer(EP_BULK_IN, OFF_TRBIN);
	if (r >= 0) {
		in_busy = 1;
		ring_tail = tail + n;
	} else {
		start_failed("bulk-in", r);
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
static int op2usb_ioctl(dev_t dev, u_long cmd, caddr_t data, int fflag, struct proc *p);

static void
tty_defaults(void)
{
	if (tty_termios_set)
		return;
	memset(&tty_termios, 0, sizeof tty_termios);
	tty_termios.c_iflag = TTYDEF_IFLAG;
	tty_termios.c_oflag = TTYDEF_OFLAG;
	tty_termios.c_cflag = TTYDEF_CFLAG;
	tty_termios.c_lflag = TTYDEF_LFLAG;
	memcpy(tty_termios.c_cc, ttydefchars, sizeof tty_termios.c_cc);
	tty_termios.c_ispeed = tty_termios.c_ospeed = B115200;
	tty_termios_set = 1;
}

/* direct ioctl round-trips on every open, before dash probes: termios store/read-back, winsize
 * store/read-back, the lazy fg default (must be the opener's own group: dash SIGTTIN-stops itself
 * if tcgetpgrp answers anything else), bogus pgid refused, live pgid accepted. One line. */
static void
tty_selftest(struct proc *p)
{
	struct termios t;
	struct winsize ws;
	int grp, err = 0;
	tty_fg = 0;
	tty_fg_logged = 1;   /* the test's own TIOCSPGRP stays quiet; the shell's still logs */
	memset(&t, 0, sizeof t);
	t.c_ispeed = t.c_ospeed = 9600;
	t.c_lflag = ECHO;
	if (op2usb_ioctl(0, TIOCSETA_64, (caddr_t)&t, 0, p) != 0)
		err |= 1;
	memset(&t, 0, sizeof t);
	if (op2usb_ioctl(0, TIOCGETA_64, (caddr_t)&t, 0, p) != 0 || t.c_ospeed != 9600 || t.c_lflag != ECHO)
		err |= 2;
	tty_termios_set = 0;
	tty_defaults();
	if (tty_termios.c_ispeed != B115200 || !(tty_termios.c_lflag & ICANON))
		err |= 4;
	ws.ws_row = 50;
	ws.ws_col = 132;
	ws.ws_xpixel = ws.ws_ypixel = 0;
	if (op2usb_ioctl(0, TIOCSWINSZ, (caddr_t)&ws, 0, p) != 0)
		err |= 8;
	memset(&ws, 0, sizeof ws);
	if (op2usb_ioctl(0, TIOCGWINSZ, (caddr_t)&ws, 0, p) != 0 || ws.ws_row != 50 || ws.ws_col != 132)
		err |= 0x10;
	tty_winsize.ws_row = 24;
	tty_winsize.ws_col = 80;
	tty_winsize.ws_xpixel = tty_winsize.ws_ypixel = 0;
	if (p == NULL) {
		err |= 0x20;
	} else {
		struct pgrp *pg = proc_pgrp(p);
		int mine = pg != PGRP_NULL ? pg->pg_id : -1;
		if (pg != PGRP_NULL)
			pg_rele(pg);
		grp = 0;
		if (op2usb_ioctl(0, TIOCGPGRP, (caddr_t)&grp, 0, p) != 0 || grp != mine)
			err |= 0x20;
		grp = 0x7fffffff;
		if (op2usb_ioctl(0, TIOCSPGRP, (caddr_t)&grp, 0, p) != EINVAL)
			err |= 0x40;
		grp = mine;
		if (op2usb_ioctl(0, TIOCSPGRP, (caddr_t)&grp, 0, p) != 0)
			err |= 0x80;
		grp = 0;
		if (op2usb_ioctl(0, TIOCGPGRP, (caddr_t)&grp, 0, p) != 0 || grp != mine)
			err |= 0x100;
	}
	tty_fg = 0;   /* leave unset: the shell's first tcgetpgrp defaults to its own group */
	tty_fg_logged = 0;
	printf("OP2 USB: tty self-test: %s (err 0x%x)\n", err ? "FAIL" : "PASS", err);
}

static int
op2usb_open(__unused dev_t dev, __unused int flags, __unused int devtype, struct proc *p)
{
	line_len = 0;
	in_tail = in_head;
	in_eof = 0;
	shell_open++;
	if (p)
		shell_pid = proc_pid(p);
	tty_selftest(p);
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
op2usb_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int fflag, struct proc *p)
{
	/* plain TIOCGETA/TIOCSETA == the _64 forms on LP64 (tty.c switches on the _64 names too);
	 * no _32 forms: there are no 32-bit processes. data is already in kernel memory. */
	tty_defaults();
	switch (cmd) {
	case TIOCGETA_64:
		memcpy(data, &tty_termios, sizeof tty_termios);
		return 0;
	case TIOCSETA_64:
	case TIOCSETAW_64:
	case TIOCSETAF_64:
		memcpy(&tty_termios, data, sizeof tty_termios);
		return 0;
	case TIOCGWINSZ:
		memcpy(data, &tty_winsize, sizeof tty_winsize);
		return 0;
	case TIOCSWINSZ:
		memcpy(&tty_winsize, data, sizeof tty_winsize);
		return 0;
	case TIOCGPGRP: {
		if (p == NULL)
			return EINVAL;
		if (tty_fg == 0) {
			struct pgrp *pg = proc_pgrp(p);
			if (pg == PGRP_NULL)
				return EINVAL;
			tty_fg = pg->pg_id;
			pg_rele(pg);
		}
		memcpy(data, &tty_fg, sizeof tty_fg);
		return 0;
	}
	case TIOCSPGRP: {
		struct pgrp *pg;
		struct session *caller;
		int want, err = 0;
		if (p == NULL)
			return EINVAL;
		memcpy(&want, data, sizeof want);
		pg = want > 0 ? pgfind(want) : PGRP_NULL;
		if (pg == PGRP_NULL)
			return EINVAL;
		caller = proc_session(p);
		if (caller == SESSION_NULL || pg->pg_session != caller) {
			err = EPERM;
		} else {
			tty_fg = pg->pg_id;
			if (!tty_fg_logged) {
				tty_fg_logged = 1;
				printf("OP2 USB: /dev/op2usb fg pgrp %d (pid %d)\n", tty_fg, proc_pid(p));
			}
		}
		if (caller != SESSION_NULL)
			session_rele(caller);
		pg_rele(pg);
		return err;
	}
	default:
		return ENOTTY;
	}
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
	.d_type = D_TTY,   /* FIODTYPE answers this (vfs_vnops.c); iOS 12 isatty is true iff it is D_TTY */
};

__attribute__((noreturn)) static void
op2_usb_thread(__unused void *param, __unused wait_result_t wr)
{
	ecm_desc_check();
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
			if (ecm_ifp == NULL)   /* root mounted: dlil is up (bsd_init.c), ue0 is safe now */
				ecm_if_create();
		}
		if (n == 30000)
			printf("OP2 USB: 30 s: configured %d, line %u baud, %u bytes in\n", usb_configured,
			    line_coding[0] | line_coding[1] << 8 | line_coding[2] << 16, (uint32_t)in_head);
		if (n > 0 && (n % 60000) == 0)   /* late state survives the ramoops wrap */
			printf("OP2 USB: %us: cfg %d alt %d desc 0x%x setup %u sf %u rx %llu tx %llu\n", n / 1000,
			    usb_configured, ecm_alt, ecm_desc_err, ev_logged, start_fail, ecm_rx_frames,
			    ecm_tx_frames);
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
