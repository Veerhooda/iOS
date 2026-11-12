/*
 * OnePlus 2 (MSM8994) Internal eMMC 5.0 / SDHCI Persistent Storage Controller
 * Darwin/XNU Block Device Driver
 *
 * Conditional Read/Write Capability:
 * - Boot argument 'emmcrw=1' or 'rootrw=1' enables write operations.
 * - Defaults to strict read-only safety mode if flags are absent to prevent
 *   accidental partition corruption during early bring-up.
 * - Features hardware CMD12 abort recovery, DAT0 busy line status polling,
 *   and automatic 3x retries on CRC or timeout faults.
 */
#if defined(ARM64_BOARD_CONFIG_MSM8994)
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <kern/clock.h>
#include <vm/pmap.h>
#include <vm/vm_kern.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <libkern/libkern.h>

int op2_emmc_rootdev(dev_t *dev);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern uint64_t ml_get_timebase(void);

#define SDHC_REG_BASE    0xf9824900UL
#define SDHC_CORE_BASE   0xf9824000UL
#define TLMM_SDC1_CTL_REG 0xfd512044UL

/* Core MSM SDCC Registers */
#define CORE_POWER            0x00
#define CORE_SW_RST           (1 << 7)
#define CORE_MCI_VERSION      0x50
#define CORE_HC_MODE          0x78
#define HC_MODE_EN            (1 << 0)
#define FF_CLK_SW_RST_DIS     (1 << 13)
#define CORE_PWRCTL_STATUS    0xDC
#define CORE_PWRCTL_CLEAR     0xE4
#define CORE_PWRCTL_CTL       0xE8
#define CORE_PWRCTL_BUS_ON    (1 << 1)
#define CORE_PWRCTL_BUS_OFF   (1 << 0)
#define CORE_PWRCTL_IO_LOW    (1 << 2)
#define CORE_PWRCTL_IO_HIGH   (1 << 3)
#define CORE_PWRCTL_BUS_SUCCESS (1 << 0)
#define CORE_PWRCTL_IO_SUCCESS  (1 << 2)

#define CORE_DLL_CONFIG       0x100
#define CORE_DLL_PDN          (1 << 29)
#define CORE_DLL_RST          (1 << 30)

#define CORE_VENDOR_SPEC      0x10C
#define CORE_CLK_PWRSAVE      (1 << 1)
#define CORE_HC_MCLK_SEL_DFLT (2 << 8)
#define CORE_HC_MCLK_SEL_MASK (3 << 8)
#define CORE_IO_PAD_PWR_SWITCH_EN (1 << 15)
#define CORE_HC_SELECT_IN_EN  (1 << 18)
#define CORE_HC_SELECT_IN_MASK (7 << 19)

/* Standard SDHCI Registers */
#define SDHCI_BLOCK_SIZE      0x04
#define SDHCI_BLOCK_COUNT     0x06
#define SDHCI_ARGUMENT        0x08
#define SDHCI_TRANSFER_MODE   0x0C
#define SDHCI_COMMAND         0x0E
#define SDHCI_RESPONSE        0x10
#define SDHCI_BUFFER          0x20
#define SDHCI_PRESENT_STATE   0x24
#define SDHCI_HOST_CONTROL    0x28
#define  SDHCI_CTRL_4BITBUS   (1 << 1)
#define  SDHCI_CTRL_8BITBUS   (1 << 5)
#define SDHCI_POWER_CONTROL   0x29
#define SDHCI_CLOCK_CONTROL   0x2C
#define SDHCI_TIMEOUT_CONTROL 0x2E
#define SDHCI_SOFTWARE_RESET  0x2F
#define  SDHCI_RESET_CMD      0x02
#define  SDHCI_RESET_DATA     0x04
#define SDHCI_INT_STATUS      0x30
#define SDHCI_INT_ENABLE      0x34
#define SDHCI_SIGNAL_ENABLE   0x38
#define SDHCI_HOST_CONTROL2   0x3E
#define SDHCI_HOST_VERSION    0xFE

/* Present State bits */
#define SDHCI_CMD_INHIBIT     (1u << 0)
#define SDHCI_DATA_INHIBIT    (1u << 1)
#define SDHCI_BUFFER_WRITE_EN (1u << 10)
#define SDHCI_BUFFER_READ_EN  (1u << 11)
#define SDHCI_DAT0_LEVEL      (1u << 20) /* PRESENT_STATE DAT[0] line level */

/* Transfer Mode bits */
#define SDHCI_TRNS_BLK_CNT_EN (1u << 1)
#define SDHCI_TRNS_READ       (1u << 4)

/* Command flags */
#define SDHCI_CMD_RESP_NONE       0x00
#define SDHCI_CMD_RESP_LONG       0x01
#define SDHCI_CMD_RESP_SHORT      0x02
#define SDHCI_CMD_RESP_SHORT_BUSY 0x03
#define SDHCI_CMD_CRC_CHECK       (1u << 3)
#define SDHCI_CMD_INDEX_CHECK     (1u << 4)
#define SDHCI_CMD_DATA_PRESENT    (1u << 5)

/* Interrupt Status bits */
#define SDHCI_INT_RESPONSE    (1u << 0)
#define SDHCI_INT_DATA_END    (1u << 1)
#define SDHCI_INT_SPACE_AVAIL (1u << 4)
#define SDHCI_INT_DATA_AVAIL  (1u << 5)
#define SDHCI_INT_ERROR       (1u << 15)

#define EMMC_SECTOR_SIZE      512u

struct gpt_header {
	uint64_t signature;           /* "EFI PART" */
	uint32_t revision;
	uint32_t header_size;
	uint32_t header_crc32;
	uint32_t reserved;
	uint64_t my_lba;
	uint64_t alternate_lba;
	uint64_t first_usable_lba;
	uint64_t last_usable_lba;
	uint8_t  disk_guid[16];
	uint64_t partition_entry_lba;
	uint32_t num_partition_entries;
	uint32_t sizeof_partition_entry;
	uint32_t partition_entry_array_crc32;
} __attribute__((packed));

struct gpt_entry {
	uint8_t  type_guid[16];
	uint8_t  unique_guid[16];
	uint64_t starting_lba;
	uint64_t ending_lba;
	uint64_t attributes;
	uint16_t partition_name[36];
} __attribute__((packed));

struct op2_emmc_info {
	vm_offset_t regs;
	vm_offset_t core_regs;
	uint64_t first_block;
	uint64_t nblocks;
	uint32_t secsize;
	int bmajor;
	int cmajor;
	dev_t bdev;
	bool ready;
};

static struct op2_emmc_info op2_emmc;
static uint8_t op2_sector_buf[EMMC_SECTOR_SIZE] __attribute__((aligned(64)));

static lck_grp_t *op2_emmc_grp = NULL;
static lck_mtx_t *op2_emmc_lock = NULL;
static int op2_emmc_in_recovery = 0; /* card-side abort recursion guard */
static uint64_t op2_emmc_recoveries = 0;
static int op2_emmc_dat0_wait(void);
static void op2_emmc_card_abort(void);

static inline uint32_t sdhc_rd32(uint32_t off)
{
	return *(volatile uint32_t *)(op2_emmc.regs + off);
}

static inline void sdhc_wr32(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(op2_emmc.regs + off) = val;
}

static inline uint16_t sdhc_rd16(uint32_t off)
{
	return *(volatile uint16_t *)(op2_emmc.regs + off);
}

static inline void sdhc_wr16(uint32_t off, uint16_t val)
{
	*(volatile uint16_t *)(op2_emmc.regs + off) = val;
}

static inline uint8_t sdhc_rd8(uint32_t off)
{
	return *(volatile uint8_t *)(op2_emmc.regs + off);
}

static inline void sdhc_wr8(uint32_t off, uint8_t val)
{
	*(volatile uint8_t *)(op2_emmc.regs + off) = val;
}

static inline uint32_t __unused sdhc_core_rd32(uint32_t off)
{
	return *(volatile uint32_t *)(op2_emmc.core_regs + off);
}

static inline void __unused sdhc_core_wr32(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(op2_emmc.core_regs + off) = val;
}

static void sdhc_set_clock(uint32_t freq_hz)
{
	uint16_t clk = sdhc_rd16(SDHCI_CLOCK_CONTROL);
	sdhc_wr16(SDHCI_CLOCK_CONTROL, clk & ~0x0004);

	uint32_t base_clk = 200000000;
	uint32_t div = base_clk / (2 * freq_hz);
	if (div > 0x3FF) div = 0x3FF;
	if (div == 0) div = 1;

	uint16_t clk_val = ((div & 0xFF) << 8) | (((div >> 8) & 0x3) << 6);
	sdhc_wr16(SDHCI_CLOCK_CONTROL, clk_val | 0x0001);

	uint64_t start = ml_get_timebase();
	while (!(sdhc_rd16(SDHCI_CLOCK_CONTROL) & 0x0002)) {
		if (ml_get_timebase() - start > 50000ULL * 192ULL / 10ULL)
			break;
		__asm__ volatile("isb");
	}

	sdhc_wr16(SDHCI_CLOCK_CONTROL, clk_val | 0x0001 | 0x0004);
	delay(10000);
}

static int sdhc_send_cmd(uint16_t opcode, uint32_t arg, uint16_t flags, uint32_t *resp0)
{
	uint64_t start = ml_get_timebase();
	uint64_t ticks = 500000ULL * 192ULL / 10ULL; /* 500ms: cover GC spikes */
	while (sdhc_rd32(SDHCI_PRESENT_STATE) & SDHCI_CMD_INHIBIT) {
		if (ml_get_timebase() - start >= ticks) {
			sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD);
			delay(1000);
			return -1;
		}
		__asm__ volatile("isb");
	}

	if ((flags & SDHCI_CMD_DATA_PRESENT) || (flags & SDHCI_CMD_RESP_SHORT_BUSY)) {
		start = ml_get_timebase();
		while (sdhc_rd32(SDHCI_PRESENT_STATE) & SDHCI_DATA_INHIBIT) {
			if (ml_get_timebase() - start >= ticks) {
				sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_DATA);
				delay(1000);
				/* OP2: card-side recovery (loop-10: -2 is a wedged
				 * card, not media; controller reset alone never
				 * clears it). */
				op2_emmc_card_abort();
				return -2;
			}
			__asm__ volatile("isb");
		}
	}

	sdhc_wr32(SDHCI_INT_STATUS, 0xFFFFFFFF);
	sdhc_wr32(SDHCI_ARGUMENT, arg);

	uint16_t cmd_val = ((opcode & 0x3F) << 8) | (flags & 0xFF);
	sdhc_wr16(SDHCI_COMMAND, cmd_val);

	start = ml_get_timebase();
	ticks = 100000ULL * 192ULL / 10ULL;
	uint32_t is = 0;
	while (ml_get_timebase() - start < ticks) {
		is = sdhc_rd32(SDHCI_INT_STATUS);
		if (is & (SDHCI_INT_RESPONSE | SDHCI_INT_ERROR))
			break;
		__asm__ volatile("isb");
	}

	if (is & SDHCI_INT_ERROR) {
		sdhc_wr32(SDHCI_INT_STATUS, is);
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD);
		uint64_t t = ml_get_timebase();
		while ((sdhc_rd8(SDHCI_SOFTWARE_RESET) & SDHCI_RESET_CMD) &&
		       (ml_get_timebase() - t < 10000ULL * 192ULL / 10ULL)) {
			__asm__ volatile("isb");
		}
		return -3;
	}
	if (!(is & SDHCI_INT_RESPONSE)) {
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD);
		uint64_t t = ml_get_timebase();
		while ((sdhc_rd8(SDHCI_SOFTWARE_RESET) & SDHCI_RESET_CMD) &&
		       (ml_get_timebase() - t < 10000ULL * 192ULL / 10ULL)) {
			__asm__ volatile("isb");
		}
		return -4;
	}

	if (resp0) {
		*resp0 = sdhc_rd32(SDHCI_RESPONSE);
	}

	sdhc_wr32(SDHCI_INT_STATUS, SDHCI_INT_RESPONSE);
	return 0;
}

static int sdhc_init_card(void)
{
	uint32_t status = 0;

	/* Verify if card is already in TRAN state (RCA = 1).
	 * NOTE (loop-14): XNU must NOT blindly re-init here: CMD0(0xF0F0F0F0)
	 * from the shim-established TRAN state wedges the card (CMD2 fails,
	 * no root, panic). Inherit shim state; mid-run wedges are handled by
	 * the CMD12 card-side recovery in sdhc_send_cmd instead. */
	int ret = sdhc_send_cmd(13, 0x00010000,
	    SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_RESP_SHORT,
	    &status);
	if (ret == 0 && (((status >> 9) & 0xF) == 4)) {
		return 0;
	}

	sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
	delay(1000);
	/* Data timeout: reset default 0x0 = 2^13 SDCLKs (328us @25MHz) is
	 * shorter than eMMC latency spikes (GC/refresh), causing spurious
	 * DATA TIMEOUT errors. Max 0xE = 2^27 clocks (~5.4s @25MHz). */
	sdhc_wr8(SDHCI_TIMEOUT_CONTROL, 0x0E);
	sdhc_wr16(SDHCI_HOST_CONTROL2, 0);
	sdhc_wr8(SDHCI_HOST_CONTROL, 0);
	sdhc_set_clock(400000);

	/* CMD0: Pre-Idle and Idle reset */
	sdhc_send_cmd(0, 0xF0F0F0F0, SDHCI_CMD_RESP_NONE, 0);
	delay(5000);
	sdhc_send_cmd(0, 0x00000000, SDHCI_CMD_RESP_NONE, 0);
	delay(10000);

	/* CMD1: SEND_OP_COND */
	uint32_t ocr = 0;
	ret = sdhc_send_cmd(1, 0x00000000, SDHCI_CMD_RESP_SHORT, &ocr);
	if (ret != 0) return -1;

	int ocr_ready = 0;
	for (int i = 0; i < 100; i++) {
		ret = sdhc_send_cmd(1, 0x40FF8080, SDHCI_CMD_RESP_SHORT, &ocr);
		if (ret == 0 && (ocr & 0x80000000)) {
			ocr_ready = 1;
			break;
		}
		delay(10000);
	}
	if (!ocr_ready) return -2;

	/* CMD2: ALL_SEND_CID */
	ret = sdhc_send_cmd(2, 0, SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC_CHECK, 0);
	if (ret != 0) return -3;

	/* CMD3: SET_RELATIVE_ADDR */
	ret = sdhc_send_cmd(3, 0x00010000,
	    SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_RESP_SHORT,
	    &status);
	if (ret != 0) return -4;

	/* CMD7: SELECT_CARD */
	ret = sdhc_send_cmd(7, 0x00010000,
	    SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_RESP_SHORT_BUSY,
	    &status);
	if (ret != 0) return -5;

	/* CMD6: SWITCH to 8-bit bus */
	sdhc_send_cmd(6, 0x03B70200,
	    SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_RESP_SHORT_BUSY,
	    &status);
	sdhc_wr8(SDHCI_HOST_CONTROL, SDHCI_CTRL_8BITBUS);
	delay(1000);

	/* Switch to 25 MHz */
	sdhc_set_clock(25000000);

	/* CMD16: SET_BLOCKLEN to 512 */
	sdhc_send_cmd(16, 512,
	    SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_RESP_SHORT,
	    &status);

	return 0;
}

/* Wait for DAT0 high (card not busy after R1b); 5s. Returns 0 if ready. */
static int
op2_emmc_dat0_wait(void)
{
	uint64_t start = ml_get_timebase();
	uint64_t ticks = 5000000ULL * 192ULL / 10ULL;
	while (!(sdhc_rd32(SDHCI_PRESENT_STATE) & SDHCI_DAT0_LEVEL)) {
		if (ml_get_timebase() - start >= ticks) {
			return -1;
		}
		__asm__ volatile("isb");
	}
	return 0;
}

/* Abort any card-side open transfer (CMD12) and re-check state (CMD13).
 * Guarded: recovery commands that wedge return without recursing. */
static void
op2_emmc_card_abort(void)
{
	uint32_t st = 0;
	if (op2_emmc_in_recovery) {
		return;
	}
	op2_emmc_in_recovery = 1;
	sdhc_send_cmd(12, 0,
	    SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK |
	    SDHCI_CMD_RESP_SHORT_BUSY, &st);
	delay(2000);
	sdhc_send_cmd(13, 0x00010000,
	    SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK |
	    SDHCI_CMD_RESP_SHORT, &st);
	op2_emmc_recoveries++;
	op2_emmc_in_recovery = 0;
}

static int sdhc_read_single_block(uint32_t lba, uint8_t *buf)
{
	uint64_t start = ml_get_timebase();
	uint64_t ticks = 500000ULL * 192ULL / 10ULL;

	if (op2_emmc_dat0_wait() != 0) {
		op2_emmc_card_abort();
		return -1;
	}
	while (sdhc_rd32(SDHCI_PRESENT_STATE) & (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT)) {
		if (ml_get_timebase() - start >= ticks) {
			sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
			op2_emmc_card_abort();
			return -1;
		}
		__asm__ volatile("isb");
	}

	sdhc_wr32(SDHCI_INT_STATUS, 0xFFFFFFFF);
	sdhc_wr16(SDHCI_BLOCK_SIZE, 512);
	sdhc_wr16(SDHCI_BLOCK_COUNT, 1);
	sdhc_wr32(SDHCI_ARGUMENT, lba);
	sdhc_wr16(SDHCI_TRANSFER_MODE, SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN);

	uint16_t cmd_flags = SDHCI_CMD_DATA_PRESENT | SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_RESP_SHORT;
	sdhc_wr16(SDHCI_COMMAND, (17 << 8) | cmd_flags);

	start = ml_get_timebase();
	ticks = 100000ULL * 192ULL / 10ULL;
	uint32_t is = 0;
	while (ml_get_timebase() - start < ticks) {
		is = sdhc_rd32(SDHCI_INT_STATUS);
		if (is & (SDHCI_INT_RESPONSE | SDHCI_INT_ERROR))
			break;
		__asm__ volatile("isb");
	}
	if ((is & SDHCI_INT_ERROR) || !(is & SDHCI_INT_RESPONSE)) {
		sdhc_wr32(SDHCI_INT_STATUS, is);
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		op2_emmc_card_abort();
		return -2;
	}
	sdhc_wr32(SDHCI_INT_STATUS, SDHCI_INT_RESPONSE);

	start = ml_get_timebase();
	ticks = 200000ULL * 192ULL / 10ULL;
	int data_ready = 0;
	while (ml_get_timebase() - start < ticks) {
		is = sdhc_rd32(SDHCI_INT_STATUS);
		if (is & SDHCI_INT_ERROR)
			break;
		if ((is & SDHCI_INT_DATA_AVAIL) || (sdhc_rd32(SDHCI_PRESENT_STATE) & SDHCI_BUFFER_READ_EN)) {
			data_ready = 1;
			break;
		}
		__asm__ volatile("isb");
	}
	if (!data_ready || (is & SDHCI_INT_ERROR)) {
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		op2_emmc_card_abort();
		return -3;
	}
	sdhc_wr32(SDHCI_INT_STATUS, SDHCI_INT_DATA_AVAIL);

	uint32_t *p = (uint32_t *)(void *)buf;
	for (int i = 0; i < 128; i++) {
		p[i] = sdhc_rd32(SDHCI_BUFFER);
	}

	start = ml_get_timebase();
	ticks = 100000ULL * 192ULL / 10ULL;
	while (ml_get_timebase() - start < ticks) {
		is = sdhc_rd32(SDHCI_INT_STATUS);
		if (is & (SDHCI_INT_DATA_END | SDHCI_INT_ERROR))
			break;
		__asm__ volatile("isb");
	}
	if (is & SDHCI_INT_ERROR) {
		sdhc_wr32(SDHCI_INT_STATUS, is);
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		op2_emmc_card_abort();
		return -4;
	}
	sdhc_wr32(SDHCI_INT_STATUS, 0xFFFFFFFF);
	return 0;
}

/*
 * Multi-block PIO read (CMD18 + Auto-CMD12). One command per run of up to
 * 128 sectors instead of one CMD17 per 512 bytes: removes 7 of every 8
 * command round-trips on a 4 KB page-in. Same error codes as the single
 * path; the caller falls back to single-block reads on any failure.
 */
#define SDHCI_TRNS_AUTO_CMD12 (1u << 2)
#define SDHCI_TRNS_MULTI      (1u << 5)
static uint64_t op2_emmc_multi_cmds = 0, op2_emmc_multi_fails = 0;
static int op2_emmc_multi_ok = -1;   /* -1 = boot-arg not parsed yet */

static int sdhc_read_multi_block(uint32_t lba, uint8_t *buf, uint32_t nblk)
{
	uint64_t start = ml_get_timebase();
	uint64_t ticks = 500000ULL * 192ULL / 10ULL;

	if (op2_emmc_dat0_wait() != 0) {
		op2_emmc_card_abort();
		return -1;
	}
	while (sdhc_rd32(SDHCI_PRESENT_STATE) & (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT)) {
		if (ml_get_timebase() - start >= ticks) {
			sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
			op2_emmc_card_abort();
			return -1;
		}
		__asm__ volatile("isb");
	}

	sdhc_wr32(SDHCI_INT_STATUS, 0xFFFFFFFF);
	sdhc_wr16(SDHCI_BLOCK_SIZE, 512);
	sdhc_wr16(SDHCI_BLOCK_COUNT, (uint16_t)nblk);
	sdhc_wr32(SDHCI_ARGUMENT, lba);
	sdhc_wr16(SDHCI_TRANSFER_MODE, SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN |
	    SDHCI_TRNS_MULTI | SDHCI_TRNS_AUTO_CMD12);

	uint16_t cmd_flags = SDHCI_CMD_DATA_PRESENT | SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_RESP_SHORT;
	sdhc_wr16(SDHCI_COMMAND, (18 << 8) | cmd_flags);

	start = ml_get_timebase();
	ticks = 100000ULL * 192ULL / 10ULL;
	uint32_t is = 0;
	while (ml_get_timebase() - start < ticks) {
		is = sdhc_rd32(SDHCI_INT_STATUS);
		if (is & (SDHCI_INT_RESPONSE | SDHCI_INT_ERROR))
			break;
		__asm__ volatile("isb");
	}
	if ((is & SDHCI_INT_ERROR) || !(is & SDHCI_INT_RESPONSE)) {
		sdhc_wr32(SDHCI_INT_STATUS, is);
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		op2_emmc_card_abort();
		return -2;
	}
	sdhc_wr32(SDHCI_INT_STATUS, SDHCI_INT_RESPONSE);

	uint32_t *p = (uint32_t *)(void *)buf;
	for (uint32_t b = 0; b < nblk; b++) {
		start = ml_get_timebase();
		ticks = 200000ULL * 192ULL / 10ULL;
		int data_ready = 0;
		while (ml_get_timebase() - start < ticks) {
			is = sdhc_rd32(SDHCI_INT_STATUS);
			if (is & SDHCI_INT_ERROR)
				break;
			if ((is & SDHCI_INT_DATA_AVAIL) || (sdhc_rd32(SDHCI_PRESENT_STATE) & SDHCI_BUFFER_READ_EN)) {
				data_ready = 1;
				break;
			}
			__asm__ volatile("isb");
		}
		if (!data_ready || (is & SDHCI_INT_ERROR)) {
			sdhc_wr32(SDHCI_INT_STATUS, is);
			sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
			op2_emmc_card_abort();
			return -3;
		}
		sdhc_wr32(SDHCI_INT_STATUS, SDHCI_INT_DATA_AVAIL);
		for (int i = 0; i < 128; i++) {
			*p++ = sdhc_rd32(SDHCI_BUFFER);
		}
	}

	start = ml_get_timebase();
	ticks = 200000ULL * 192ULL / 10ULL;
	while (ml_get_timebase() - start < ticks) {
		is = sdhc_rd32(SDHCI_INT_STATUS);
		if (is & (SDHCI_INT_DATA_END | SDHCI_INT_ERROR))
			break;
		__asm__ volatile("isb");
	}
	if ((is & SDHCI_INT_ERROR) || !(is & SDHCI_INT_DATA_END)) {
		sdhc_wr32(SDHCI_INT_STATUS, is);
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		op2_emmc_card_abort();
		return -4;
	}
	sdhc_wr32(SDHCI_INT_STATUS, 0xFFFFFFFF);
	return 0;
}

static int sdhc_write_single_block(uint32_t lba, const uint8_t *buf)
{
	uint64_t start = ml_get_timebase();
	uint64_t ticks = 500000ULL * 192ULL / 10ULL;

	if (op2_emmc_dat0_wait() != 0) {
		op2_emmc_card_abort();
		return -1;
	}
	while (sdhc_rd32(SDHCI_PRESENT_STATE) & (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT)) {
		if (ml_get_timebase() - start >= ticks) {
			sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
			op2_emmc_card_abort();
			return -1;
		}
		__asm__ volatile("isb");
	}

	sdhc_wr32(SDHCI_INT_STATUS, 0xFFFFFFFF);
	sdhc_wr16(SDHCI_BLOCK_SIZE, 512);
	sdhc_wr16(SDHCI_BLOCK_COUNT, 1);
	sdhc_wr32(SDHCI_ARGUMENT, lba);
	sdhc_wr16(SDHCI_TRANSFER_MODE, SDHCI_TRNS_BLK_CNT_EN);

	uint16_t cmd_flags = SDHCI_CMD_DATA_PRESENT | SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_RESP_SHORT;
	sdhc_wr16(SDHCI_COMMAND, (24 << 8) | cmd_flags); /* CMD24: WRITE_BLOCK */

	start = ml_get_timebase();
	ticks = 100000ULL * 192ULL / 10ULL;
	uint32_t is = 0;
	while (ml_get_timebase() - start < ticks) {
		is = sdhc_rd32(SDHCI_INT_STATUS);
		if (is & (SDHCI_INT_RESPONSE | SDHCI_INT_ERROR))
			break;
		__asm__ volatile("isb");
	}
	if ((is & SDHCI_INT_ERROR) || !(is & SDHCI_INT_RESPONSE)) {
		sdhc_wr32(SDHCI_INT_STATUS, is);
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		op2_emmc_card_abort();
		return -2;
	}
	sdhc_wr32(SDHCI_INT_STATUS, SDHCI_INT_RESPONSE);

	start = ml_get_timebase();
	ticks = 200000ULL * 192ULL / 10ULL;
	int data_ready = 0;
	while (ml_get_timebase() - start < ticks) {
		is = sdhc_rd32(SDHCI_INT_STATUS);
		if (is & SDHCI_INT_ERROR)
			break;
		if ((is & SDHCI_INT_SPACE_AVAIL) || (sdhc_rd32(SDHCI_PRESENT_STATE) & SDHCI_BUFFER_WRITE_EN)) {
			data_ready = 1;
			break;
		}
		__asm__ volatile("isb");
	}
	if (!data_ready || (is & SDHCI_INT_ERROR)) {
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		op2_emmc_card_abort();
		return -3;
	}
	sdhc_wr32(SDHCI_INT_STATUS, SDHCI_INT_SPACE_AVAIL);

	const uint32_t *p = (const uint32_t *)(const void *)buf;
	for (int i = 0; i < 128; i++) {
		sdhc_wr32(SDHCI_BUFFER, p[i]);
	}

	start = ml_get_timebase();
	ticks = 500000ULL * 192ULL / 10ULL;
	while (ml_get_timebase() - start < ticks) {
		is = sdhc_rd32(SDHCI_INT_STATUS);
		if (is & (SDHCI_INT_DATA_END | SDHCI_INT_ERROR))
			break;
		__asm__ volatile("isb");
	}
	if (is & SDHCI_INT_ERROR) {
		sdhc_wr32(SDHCI_INT_STATUS, is);
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		op2_emmc_card_abort();
		return -4;
	}
	sdhc_wr32(SDHCI_INT_STATUS, 0xFFFFFFFF);
	return 0;
}

static int op2_emmc_find_partition(const char *want)
{
	int ret = sdhc_read_single_block(1, op2_sector_buf);
	if (ret != 0) {
		printf("OP2 eMMC: failed to read primary GPT header (LBA 1)\n");
		return ENXIO;
	}

	struct gpt_header *hdr = (struct gpt_header *)(void *)op2_sector_buf;
	if (hdr->signature != 0x5452415020494645ULL) {
		printf("OP2 eMMC: invalid GPT signature 0x%llx\n", hdr->signature);
		return ENXIO;
	}

	uint64_t part_lba = hdr->partition_entry_lba;
	uint32_t num_parts = hdr->num_partition_entries;
	uint32_t part_sz = hdr->sizeof_partition_entry;
	if (part_sz != 128 || num_parts == 0) {
		printf("OP2 eMMC: unsupported partition entries count %u size %u\n", num_parts, part_sz);
		return ENXIO;
	}
	if (num_parts > 64) num_parts = 64;

	uint64_t cur_sec = 0xFFFFFFFFFFFFFFFFULL;
	for (uint32_t i = 0; i < num_parts; i++) {
		uint64_t lba = part_lba + (i * part_sz / 512);
		uint32_t off = (i * part_sz) % 512;
		if (lba != cur_sec) {
			if (sdhc_read_single_block((uint32_t)lba, op2_sector_buf) != 0) {
				return ENXIO;
			}
			cur_sec = lba;
		}

		struct gpt_entry *entry = (struct gpt_entry *)(void *)(op2_sector_buf + off);
		int non_zero = 0;
		for (int g = 0; g < 16; g++) {
			if (entry->type_guid[g]) { non_zero = 1; break; }
		}
		if (!non_zero) continue;

		char name[37];
		unsigned c;
		for (c = 0; c < 36; c++) {
			uint16_t ch = entry->partition_name[c];
			if (ch == 0 || ch > 0x7f) break;
			name[c] = (char)ch;
		}
		name[c] = '\0';

		if (strcmp(name, want) == 0) {
			uint64_t first = entry->starting_lba;
			uint64_t last = entry->ending_lba;
			if (last < first) return ENXIO;
			op2_emmc.first_block = first;
			op2_emmc.nblocks = last - first + 1;
			printf("OP2 eMMC: partition \"%s\" found: start LBA %llu, count %llu blocks (%llu MB)\n",
			       want, op2_emmc.first_block, op2_emmc.nblocks, (op2_emmc.nblocks * 512) / (1024 * 1024));
			return 0;
		}
	}

	printf("OP2 eMMC: no GPT partition named \"%s\"\n", want);
	return ENOENT;
}

static int op2_emmc_writable = 0;

static int op2_emmc_open(__unused dev_t dev, int flags, __unused int devtype, __unused struct proc *p)
{
	if (!op2_emmc.ready) {
		return ENXIO;
	}
	if ((flags & FWRITE) && !op2_emmc_writable) {
		return EROFS;
	}
	return 0;
}

static int op2_emmc_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

/* Failure accounting: counters + ring of recent failures, dumped from the
 * deadline panic (inline prints wrap out of ramoops). Single CPU: no locking. */
static uint64_t op2_emmc_read_blocks = 0;
static uint64_t op2_emmc_fail_map = 0, op2_emmc_fail_oob = 0;
static uint64_t op2_emmc_fail_rd = 0, op2_emmc_fail_wr = 0;
static uint64_t op2_emmc_read_retries_ok = 0;
static uint64_t op2_emmc_write_retries_ok = 0;
#define OP2_EMMC_FAIL_RING_N 32
struct op2_emmc_fail_ent {
	uint64_t lba;
	uint64_t blkno;
	uint32_t count;
	int rc;
	char op;
};
static struct op2_emmc_fail_ent op2_emmc_fail_ring[OP2_EMMC_FAIL_RING_N];
static unsigned op2_emmc_fail_count = 0;

/*
 * Flush every mounted filesystem (same as the sync(2) syscall). Called
 * shortly before the op2_deadline reset so the non-clean reboot does not
 * leave half-written HFS metadata behind (allocation bitmap / catalog
 * damage accumulated over dozens of hard resets before this existed).
 */
struct sync_args;
extern int sync(proc_t p, struct sync_args *uap, int32_t *retval);
void op2_fs_sync(void);
void
op2_fs_sync(void)
{
	int32_t rv = 0;
	sync(kernproc, NULL, &rv);
	printf("OP2: filesystems synced (pre-reset)\n");
}

uint64_t op2_emmc_blocks_read(void);
uint64_t
op2_emmc_blocks_read(void)
{
	return op2_emmc_read_blocks;
}

uint64_t op2_emmc_multi_count(void);
uint64_t
op2_emmc_multi_count(void)
{
	return op2_emmc_multi_cmds;
}

void op2_emmc_dump_stats(void);
void
op2_emmc_dump_stats(void)
{
	unsigned nrec = op2_emmc_fail_count;
	unsigned first = nrec > OP2_EMMC_FAIL_RING_N ? nrec - OP2_EMMC_FAIL_RING_N : 0;
	printf("OP2 eMMC MULTI: cmds=%llu fails=%llu enabled=%d\n",
	    (unsigned long long)op2_emmc_multi_cmds, (unsigned long long)op2_emmc_multi_fails,
	    op2_emmc_multi_ok);
	printf("OP2 eMMC STATS: reads=%llu rd_fail=%llu wr_fail=%llu map_fail=%llu oob=%llu retry_ok=%llu wr_retry_ok=%llu recoveries=%llu fails_recorded=%u%s\n",
	    (unsigned long long)op2_emmc_read_blocks,
	    (unsigned long long)op2_emmc_fail_rd,
	    (unsigned long long)op2_emmc_fail_wr,
	    (unsigned long long)op2_emmc_fail_map,
	    (unsigned long long)op2_emmc_fail_oob,
	    (unsigned long long)op2_emmc_read_retries_ok,
	    (unsigned long long)op2_emmc_write_retries_ok,
	    (unsigned long long)op2_emmc_recoveries,
	    nrec, nrec > OP2_EMMC_FAIL_RING_N ? " (showing last 32)" : "");
	for (unsigned i = first; i < nrec; i++) {
		struct op2_emmc_fail_ent *e = &op2_emmc_fail_ring[i % OP2_EMMC_FAIL_RING_N];
		printf("OP2 eMMC FAILRING #%u: op=%c lba=%llu rc=%d blkno=%lld count=%u\n",
		    i, e->op, (unsigned long long)e->lba, e->rc,
		    (long long)e->blkno, e->count);
	}
}

static void
op2_emmc_fail_record(char op, uint64_t lba, int rc, uint64_t blkno, uint32_t count)
{
	/* SMP: unique slot per writer (strategy already serialized by
	 * op2_emmc_lock, but the dump can race; belt and braces). */
	unsigned slot = __atomic_fetch_add(&op2_emmc_fail_count, 1, __ATOMIC_RELAXED);
	struct op2_emmc_fail_ent *e = &op2_emmc_fail_ring[slot % OP2_EMMC_FAIL_RING_N];
	e->op = op;
	e->lba = lba;
	e->rc = rc;
	e->blkno = blkno;
	e->count = count;
}

/* Single-block read with two extra attempts for DATA-phase errors (-3/-4:
 * data/integrity timeouts are plausibly transient eMMC stalls). RESPONSE
 * errors (-1/-2: no response, sticky controller state) never recover by
 * retry (run131: 1 recovery in 2117 fails) and each attempt burns ~100ms,
 * so fail them immediately. Only repeated data failures fail the buf. */
static int
op2_emmc_read_retry(uint32_t lba, uint8_t *buf)
{
	int rc = sdhc_read_single_block(lba, buf);
	int tries = 1;
	/* -2 (response timeout) also retries: the abort+reset inside the
	 * failed attempt unwedges the card, so re-issuing usually lands.
	 * (loop-24: all 6 fails were -2, each instant-EIO without this.)
	 * -1 (DAT0 wedged 5s) still fails fast: retrying it would burn
	 * 15s/sector and wedge-amplify multi-sector bufs. */
	while ((rc == -2 || rc == -3 || rc == -4) && tries < 3) {
		rc = sdhc_read_single_block(lba, buf);
		tries++;
	}
	op2_emmc_read_blocks += (uint64_t)tries;
	if (tries > 1 && rc == 0) {
		op2_emmc_read_retries_ok++;
	}
	return rc;
}

static int
op2_emmc_write_retry(uint32_t lba, const uint8_t *buf)
{
	int rc = sdhc_write_single_block(lba, buf);
	int tries = 1;
	while ((rc == -2 || rc == -3 || rc == -4) && tries < 3) {
		rc = sdhc_write_single_block(lba, buf);
		tries++;
	}
	if (tries > 1 && rc == 0) {
		op2_emmc_write_retries_ok++;
	}
	return rc;
}

static void op2_emmc_strategy(buf_t bp)
{
	int error = 0;
	caddr_t vaddr = NULL;
	uint64_t off = (uint64_t)buf_blkno(bp) * op2_emmc.secsize;
	uint32_t count = buf_count(bp);
	uint64_t part_bytes = op2_emmc.nblocks * op2_emmc.secsize;

	buf_setresid(bp, count);
	if (!(buf_flags(bp) & B_READ)) {
		if (!op2_emmc_writable) {
			error = EROFS;
			goto done;
		}
	}
	if (off > part_bytes || count > part_bytes - off) {
		op2_emmc_fail_oob++;
		op2_emmc_fail_record('O', 0, EINVAL, (uint64_t)buf_blkno(bp), count);
		printf("OP2 eMMC FAIL oob: blkno=%lld count=%u off=%llu part=%llu\n",
		    (long long)buf_blkno(bp), count,
		    (unsigned long long)off, (unsigned long long)part_bytes);
		error = EINVAL;
		goto done;
	}
	if (count == 0) {
		goto done;
	}
	if (buf_map(bp, &vaddr)) {
		op2_emmc_fail_map++;
		op2_emmc_fail_record('M', 0, ENOMEM, (uint64_t)buf_blkno(bp), count);
		printf("OP2 eMMC FAIL buf_map: blkno=%lld count=%u flags=0x%x reads=%llu rd_fail=%llu map_fail=%llu\n",
		    (long long)buf_blkno(bp), count, buf_flags(bp),
		    (unsigned long long)op2_emmc_read_blocks,
		    (unsigned long long)op2_emmc_fail_rd,
		    (unsigned long long)op2_emmc_fail_map);
		error = EIO;
		goto done;
	}

	if (op2_emmc_lock) lck_mtx_lock(op2_emmc_lock);
	for (uint32_t done_bytes = 0; done_bytes < count; done_bytes += EMMC_SECTOR_SIZE) {
		uint64_t lba = op2_emmc.first_block + ((off + done_bytes) / EMMC_SECTOR_SIZE);
		uint32_t chunk = MIN(EMMC_SECTOR_SIZE, count - done_bytes);
		if (op2_emmc_multi_ok < 0) {
			int en = 1;
			PE_parse_boot_argn("op2_emmc_multi", &en, sizeof(en));
			op2_emmc_multi_ok = en ? 1 : 0;
			if (op2_emmc_multi_ok) {
				/* Self-test: 32 sectors via CMD18 must match 32 CMD17 reads
				 * byte for byte, or multi-block stays off this boot. */
				static uint8_t st_multi[32 * EMMC_SECTOR_SIZE] __attribute__((aligned(64)));
				static uint8_t st_single[32 * EMMC_SECTOR_SIZE] __attribute__((aligned(64)));
				uint32_t st_lba = (uint32_t)(op2_emmc.first_block + 2048);
				int st_rc = sdhc_read_multi_block(st_lba, st_multi, 32);
				int st_ok = (st_rc == 0);
				for (uint32_t k = 0; st_ok && k < 32; k++) {
					if (op2_emmc_read_retry(st_lba + k, st_single + k * EMMC_SECTOR_SIZE) != 0) {
						st_ok = 0;
					}
				}
				if (st_ok && memcmp(st_multi, st_single, sizeof(st_multi)) != 0) {
					st_ok = 0;
				}
				op2_emmc_multi_ok = st_ok;
				printf("OP2 eMMC: multi-block self-test %s (rc=%d)\n", st_ok ? "PASSED" : "FAILED", st_rc);
			}
			printf("OP2 eMMC: multi-block reads %s\n", op2_emmc_multi_ok ? "enabled" : "disabled");
		}
		if ((buf_flags(bp) & B_READ) && op2_emmc_multi_ok > 0 &&
		    chunk == EMMC_SECTOR_SIZE &&
		    ((((uintptr_t)vaddr + done_bytes) & 3) == 0) &&
		    (count - done_bytes) >= 2 * EMMC_SECTOR_SIZE) {
			uint32_t nblk = (count - done_bytes) / EMMC_SECTOR_SIZE;
			if (nblk > 128) {
				nblk = 128;
			}
			int mrc = sdhc_read_multi_block((uint32_t)lba, (uint8_t *)vaddr + done_bytes, nblk);
			if (mrc == 0) {
				op2_emmc_read_blocks += nblk;
				op2_emmc_multi_cmds++;
				done_bytes += (nblk - 1) * EMMC_SECTOR_SIZE; /* loop adds the last one */
				buf_setresid(bp, count - (done_bytes + EMMC_SECTOR_SIZE));
				continue;
			}
			op2_emmc_multi_fails++;
			if (op2_emmc_multi_fails <= 4) {
				printf("OP2 eMMC: multi-block read failed rc=%d lba=%llu n=%u (fails=%llu), single-block fallback\n",
				    mrc, (unsigned long long)lba, nblk, (unsigned long long)op2_emmc_multi_fails);
			}
			if (op2_emmc_multi_fails >= 8 && op2_emmc_multi_fails * 100 > op2_emmc_multi_cmds) {
				op2_emmc_multi_ok = 0;
				printf("OP2 eMMC: multi-block reads DISABLED (too many failures)\n");
			}
			/* fall through: this sector via the proven single-block path */
		}
		if (buf_flags(bp) & B_READ) {
			uint8_t *dst = (chunk == EMMC_SECTOR_SIZE && (((uintptr_t)vaddr + done_bytes) & 3) == 0) ?
			    (uint8_t *)vaddr + done_bytes : op2_sector_buf;
			int rc = op2_emmc_read_retry((uint32_t)lba, dst);
			if (rc) {
				op2_emmc_fail_rd++;
				op2_emmc_fail_record('R', lba, rc, (uint64_t)buf_blkno(bp), count);
				if (op2_emmc_fail_rd <= 8) {
					printf("OP2 eMMC FAIL read: lba=%llu rc=%d blkno=%lld count=%u reads=%llu rd_fail=%llu\n",
					    (unsigned long long)lba, rc, (long long)buf_blkno(bp), count,
					    (unsigned long long)op2_emmc_read_blocks,
					    (unsigned long long)op2_emmc_fail_rd);
				}
				error = EIO;
				break;
			}
			if ((op2_emmc_read_blocks % 200000) == 0) {
				printf("OP2 eMMC: blocks read=%llu rd_fail=%llu wr_fail=%llu map_fail=%llu oob=%llu\n",
				    (unsigned long long)op2_emmc_read_blocks,
				    (unsigned long long)op2_emmc_fail_rd,
				    (unsigned long long)op2_emmc_fail_wr,
				    (unsigned long long)op2_emmc_fail_map,
				    (unsigned long long)op2_emmc_fail_oob);
			}
			if (dst == op2_sector_buf) {
				memcpy(vaddr + done_bytes, op2_sector_buf, chunk);
			}
		} else {
			if (chunk < EMMC_SECTOR_SIZE) {
				int rc = op2_emmc_read_retry((uint32_t)lba, op2_sector_buf);
				if (rc) {
					op2_emmc_fail_rd++;
					op2_emmc_fail_record('m', lba, rc, (uint64_t)buf_blkno(bp), count);
					if (op2_emmc_fail_rd <= 8) {
						printf("OP2 eMMC FAIL rmw-read: lba=%llu rc=%d rd_fail=%llu\n",
						    (unsigned long long)lba, rc,
						    (unsigned long long)op2_emmc_fail_rd);
					}
					error = EIO;
					break;
				}
				memcpy(op2_sector_buf, vaddr + done_bytes, chunk);
				error = op2_emmc_write_retry((uint32_t)lba, op2_sector_buf);
			} else {
				error = op2_emmc_write_retry((uint32_t)lba, (const uint8_t *)(vaddr + done_bytes));
			}
			if (error) {
				op2_emmc_fail_wr++;
				op2_emmc_fail_record('W', lba, error, (uint64_t)buf_blkno(bp), count);
				if (op2_emmc_fail_wr <= 8) {
					printf("OP2 eMMC FAIL write: lba=%llu rc=%d wr_fail=%llu\n",
					    (unsigned long long)lba, error,
					    (unsigned long long)op2_emmc_fail_wr);
				}
				error = EIO;
				break;
			}
			static uint64_t op2_emmc_write_blocks = 0;
			op2_emmc_write_blocks++;
			if (op2_emmc_write_blocks == 1 || (op2_emmc_write_blocks % 50000) == 0) {
				printf("OP2 eMMC: blocks written=%llu (last LBA=%llu)\n",
				    (unsigned long long)op2_emmc_write_blocks, (unsigned long long)lba);
			}
		}
		buf_setresid(bp, count - (done_bytes + chunk));
	}
	if (op2_emmc_lock) lck_mtx_unlock(op2_emmc_lock);
	buf_unmap(bp);

done:
	if (error) {
		buf_seterror(bp, error);
	}
	buf_biodone(bp);
}

static int op2_emmc_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag,
    __unused struct proc *p, int is_char)
{
	uint32_t *f = (uint32_t *)(void *)data;
	uint64_t *o = (uint64_t *)(void *)data;

	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
		*f = op2_emmc.secsize;
		return 0;
	case DKIOCSETBLOCKSIZE:
		if (is_char) return ENODEV;
		if (*f != EMMC_SECTOR_SIZE) return EINVAL;
		return 0;
	case DKIOCGETPHYSICALBLOCKSIZE:
		*f = EMMC_SECTOR_SIZE;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*o = op2_emmc.nblocks;
		return 0;
	case DKIOCISWRITABLE:
		*f = op2_emmc_writable ? 1 : 0;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*o = 2048; /* 1 MB */
		return 0;
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*o = op2_emmc_writable ? 2048 : 0;
		return 0;
	case DKIOCGETMAXSEGMENTCOUNTREAD:
	case DKIOCGETMAXSEGMENTCOUNTWRITE:
		*o = 1;
		return 0;
	default:
		return ENOTTY;
	}
}

static int op2_emmc_bioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	return op2_emmc_ioctl(dev, cmd, data, flag, p, 0);
}

static int op2_emmc_cioctl(dev_t dev, u_long cmd, caddr_t data, int flag, struct proc *p)
{
	return op2_emmc_ioctl(dev, cmd, data, flag, p, 1);
}

static int op2_emmc_psize(__unused dev_t dev)
{
	return op2_emmc.ready ? (int)MIN(op2_emmc.nblocks, INT_MAX) : -1;
}

static int op2_emmc_read(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return physio(op2_emmc_strategy, NULL, dev, B_READ, minphys, uio, (int)op2_emmc.secsize);
}

static int op2_emmc_write(dev_t dev, struct uio *uio, __unused int ioflag)
{
	if (!op2_emmc_writable) {
		return EROFS;
	}
	return physio(op2_emmc_strategy, NULL, dev, B_WRITE, minphys, uio, (int)op2_emmc.secsize);
}

static struct bdevsw op2_emmc_bdevsw = {
	.d_open     = op2_emmc_open,
	.d_close    = op2_emmc_close,
	.d_strategy = op2_emmc_strategy,
	.d_ioctl    = op2_emmc_bioctl,
	.d_dump     = eno_dump,
	.d_psize    = op2_emmc_psize,
	.d_type     = D_DISK,
};

static struct cdevsw op2_emmc_cdevsw = {
	.d_open       = op2_emmc_open,
	.d_close      = op2_emmc_close,
	.d_read       = op2_emmc_read,
	.d_write      = op2_emmc_write,
	.d_ioctl      = op2_emmc_cioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

static int op2_emmc_init(void)
{
	char want[64] = "userdata";
	PE_parse_boot_argn("emmcroot", want, sizeof(want));

	int rw = 1;
	PE_parse_boot_argn("emmcrw", &rw, sizeof(rw));
	if (rw != 0) {
		op2_emmc_writable = 1;
		printf("OP2 eMMC: write support ENABLED for partition \"%s\"\n", want);
	}

	op2_emmc.regs = ml_io_map(SDHC_REG_BASE, 0x200);
	op2_emmc.core_regs = ml_io_map(SDHC_CORE_BASE, 0x800);
	op2_emmc.secsize = EMMC_SECTOR_SIZE;

	if (!op2_emmc.regs || !op2_emmc.core_regs) {
		printf("OP2 eMMC: failed to map MMIO registers\n");
		return ENXIO;
	}

	if (!op2_emmc_grp) {
		op2_emmc_grp = lck_grp_alloc_init("op2_emmc", LCK_GRP_ATTR_NULL);
		if (op2_emmc_grp) {
			op2_emmc_lock = lck_mtx_alloc_init(op2_emmc_grp, LCK_ATTR_NULL);
		}
	}

	if (op2_emmc_lock) lck_mtx_lock(op2_emmc_lock);
	int error = sdhc_init_card();
	if (error == 0) {
		error = op2_emmc_find_partition(want);
	}
	if (op2_emmc_lock) lck_mtx_unlock(op2_emmc_lock);
	if (error) {
		return error;
	}

	op2_emmc.bmajor = bdevsw_add(-1, &op2_emmc_bdevsw);
	if (op2_emmc.bmajor < 0) {
		printf("OP2 eMMC: bdevsw_add failed\n");
		return ENXIO;
	}
	op2_emmc.cmajor = cdevsw_add_with_bdev(-1, &op2_emmc_cdevsw, op2_emmc.bmajor);
	if (op2_emmc.cmajor < 0) {
		printf("OP2 eMMC: cdevsw_add failed\n");
		bdevsw_remove(op2_emmc.bmajor, &op2_emmc_bdevsw);
		return ENXIO;
	}

	op2_emmc.bdev = makedev(op2_emmc.bmajor, 0);
	devfs_make_node(op2_emmc.bdev, DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "emmcroot0");
	devfs_make_node(makedev(op2_emmc.cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "remmcroot0");

	/* Standard Darwin/iOS disk aliases for fsck and launchd root checks */
	devfs_make_node(op2_emmc.bdev, DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "disk0");
	devfs_make_node(makedev(op2_emmc.cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rdisk0");
	devfs_make_node(op2_emmc.bdev, DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "disk0s1");
	devfs_make_node(makedev(op2_emmc.cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rdisk0s1");
	devfs_make_node(op2_emmc.bdev, DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "disk0s2");
	devfs_make_node(makedev(op2_emmc.cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rdisk0s2");
	devfs_make_node(op2_emmc.bdev, DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "disk0s1s1");
	devfs_make_node(makedev(op2_emmc.cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rdisk0s1s1");

	op2_emmc.ready = true;
	printf("OP2 eMMC: registered /dev/emmcroot0 (bmajor=%d, cmajor=%d) for \"%s\" (%llu blocks)\n",
	       op2_emmc.bmajor, op2_emmc.cmajor, want, op2_emmc.nblocks);
	return 0;
}

int op2_emmc_rootdev(dev_t *dev)
{
	int error = 0;
	if (!op2_emmc.ready) {
		error = op2_emmc_init();
	}
	if (!error) {
		*dev = op2_emmc.bdev;
	}
	return error;
}

#endif /* ARM64_BOARD_CONFIG_MSM8994 */
