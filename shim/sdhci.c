/*
 * OnePlus 2 (MSM8994) eMMC 5.0 / SDHCI Bare-Metal Driver
 * JEDEC eMMC Standard / SD Host Controller Standard Specification
 */
#include <stdint.h>
#include "sdhci.h"

#define SDHC_REG_BASE    0xf9824900UL
#define SDHC_CORE_BASE   0xf9824000UL

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
#define CORE_DLL_EN           (1 << 16)
#define CORE_CK_OUT_EN        (1 << 18)
#define CORE_DLL_PDN          (1 << 29)
#define CORE_DLL_RST          (1 << 30)
#define CORE_DLL_STATUS       0x108
#define CORE_DLL_LOCK         (1 << 7)

#define CORE_VENDOR_SPEC      0x10C
#define CORE_CLK_PWRSAVE      (1 << 1)
#define CORE_HC_MCLK_SEL_DFLT (2 << 8)
#define CORE_HC_MCLK_SEL_MASK (3 << 8)
#define CORE_IO_PAD_PWR_SWITCH_EN (1 << 15)
#define CORE_HC_SELECT_IN_EN  (1 << 18)
#define CORE_HC_SELECT_IN_MASK (7 << 19)

#define CORE_VENDOR_SPEC_FUNC2 0x110
#define TLMM_SDC1_CTL_REG     0xfd512044UL

/* Standard SDHCI Registers */
#define SDHCI_DMA_ADDRESS     0x00
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
#define SDHCI_BLOCK_GAP_CTRL  0x2A
#define SDHCI_WAKEUP_CTRL     0x2B
#define SDHCI_CLOCK_CONTROL   0x2C
#define SDHCI_TIMEOUT_CONTROL 0x2E
#define SDHCI_SOFTWARE_RESET  0x2F
#define  SDHCI_RESET_ALL      0x01
#define  SDHCI_RESET_CMD      0x02
#define  SDHCI_RESET_DATA     0x04
#define SDHCI_INT_STATUS      0x30
#define SDHCI_INT_ENABLE      0x34
#define SDHCI_SIGNAL_ENABLE   0x38
#define SDHCI_AUTO_CMD_STATUS 0x3C
#define SDHCI_HOST_CONTROL2   0x3E
#define SDHCI_CAPABILITIES    0x40
#define SDHCI_CAPABILITIES_1  0x44
#define SDHCI_HOST_VERSION    0xFE

/* Present State bits */
#define SDHCI_CMD_INHIBIT     (1u << 0)
#define SDHCI_DATA_INHIBIT    (1u << 1)
#define SDHCI_DAT_ACTIVE      (1u << 2)
#define SDHCI_CARD_PRESENT    (1u << 16)
#define SDHCI_BUFFER_READ_EN  (1u << 11)

/* Transfer Mode bits */
#define SDHCI_TRNS_DMA        (1u << 0)
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
#define SDHCI_INT_DATA_AVAIL  (1u << 5)
#define SDHCI_INT_ERROR       (1u << 15)

struct gpt_header {
	uint64_t signature;           /* "EFI PART" = 0x5452415020494645 */
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

static uint8_t sec_buf[512] __attribute__((aligned(64)));

static inline uint32_t sdhc_rd32(uint32_t off)
{
	return *(volatile uint32_t *)(SDHC_REG_BASE + off);
}

static inline void sdhc_wr32(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(SDHC_REG_BASE + off) = val;
}

static inline uint16_t sdhc_rd16(uint32_t off)
{
	return *(volatile uint16_t *)(SDHC_REG_BASE + off);
}

static inline void sdhc_wr16(uint32_t off, uint16_t val)
{
	*(volatile uint16_t *)(SDHC_REG_BASE + off) = val;
}

static inline uint8_t sdhc_rd8(uint32_t off)
{
	return *(volatile uint8_t *)(SDHC_REG_BASE + off);
}

static inline void sdhc_wr8(uint32_t off, uint8_t val)
{
	*(volatile uint8_t *)(SDHC_REG_BASE + off) = val;
}

static inline uint32_t sdhc_core_rd32(uint32_t off)
{
	return *(volatile uint32_t *)(SDHC_CORE_BASE + off);
}

static inline void sdhc_core_wr32(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(SDHC_CORE_BASE + off) = val;
}

static inline uint64_t get_cntvct(void)
{
	uint64_t val;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(val));
	return val;
}

static void delay_us(uint32_t us)
{
	uint64_t start = get_cntvct();
	uint64_t ticks = (uint64_t)us * 192ULL / 10ULL;
	while (get_cntvct() - start < ticks) {
		__asm__ volatile("isb");
	}
}

static void ramoops_dec(uint64_t val)
{
	char buf[24];
	int i = 0;
	if (val == 0) {
		ramoops_putc('0');
		return;
	}
	while (val > 0) {
		buf[i++] = '0' + (char)(val % 10);
		val /= 10;
	}
	while (i > 0) {
		ramoops_putc(buf[--i]);
	}
}

static void ramoops_dec_pad(uint64_t val, int width)
{
	int len = 0;
	uint64_t tmp = val;
	if (tmp == 0) len = 1;
	else {
		while (tmp > 0) {
			len++;
			tmp /= 10;
		}
	}
	for (int p = 0; p < width - len; p++)
		ramoops_putc(' ');
	ramoops_dec(val);
}

static void ramoops_str_pad(const char *s, int width)
{
	int len = 0;
	while (s[len] != '\0') {
		ramoops_putc(s[len]);
		len++;
	}
	for (int p = len; p < width; p++)
		ramoops_putc(' ');
}

static void sdhc_set_clock(uint32_t freq_hz)
{
	/* 1. Disable card clock */
	uint16_t clk = sdhc_rd16(SDHCI_CLOCK_CONTROL);
	sdhc_wr16(SDHCI_CLOCK_CONTROL, clk & ~0x0004);

	/* Base clock is 200 MHz */
	uint32_t base_clk = 200000000;
	uint32_t div = base_clk / (2 * freq_hz);
	if (div > 0x3FF) div = 0x3FF;
	if (div == 0) div = 1;

	uint16_t clk_val = ((div & 0xFF) << 8) | (((div >> 8) & 0x3) << 6);

	/* 2. Enable internal clock with new divider */
	sdhc_wr16(SDHCI_CLOCK_CONTROL, clk_val | 0x0001);

	/* 3. Wait for internal clock stable */
	uint64_t start = get_cntvct();
	while (!(sdhc_rd16(SDHCI_CLOCK_CONTROL) & 0x0002)) {
		if (get_cntvct() - start > 50000ULL * 192ULL / 10ULL)
			break;
		__asm__ volatile("isb");
	}

	/* 4. Enable card clock */
	sdhc_wr16(SDHCI_CLOCK_CONTROL, clk_val | 0x0001 | 0x0004);
	delay_us(10000); /* 10 ms */
}

static int sdhc_send_cmd(uint16_t opcode, uint32_t arg, uint16_t flags, uint32_t *resp0)
{
	/* Wait up to 50ms for CMD inhibit */
	uint64_t start = get_cntvct();
	uint64_t ticks = 50000ULL * 192ULL / 10ULL;
	while (sdhc_rd32(SDHCI_PRESENT_STATE) & SDHCI_CMD_INHIBIT) {
		if (get_cntvct() - start >= ticks) {
			ramoops_puts("  [SDHCI] send_cmd: CMD inhibit timeout!\n");
			sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD);
			delay_us(1000);
			return -1;
		}
		__asm__ volatile("isb");
	}

	if ((flags & SDHCI_CMD_DATA_PRESENT) || (flags & SDHCI_CMD_RESP_SHORT_BUSY)) {
		start = get_cntvct();
		while (sdhc_rd32(SDHCI_PRESENT_STATE) & SDHCI_DATA_INHIBIT) {
			if (get_cntvct() - start >= ticks) {
				ramoops_puts("  [SDHCI] send_cmd: DATA inhibit timeout!\n");
				sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_DATA);
				delay_us(1000);
				return -2;
			}
			__asm__ volatile("isb");
		}
	}

	/* Clear interrupt status */
	sdhc_wr32(SDHCI_INT_STATUS, 0xFFFFFFFF);

	/* Set Argument */
	sdhc_wr32(SDHCI_ARGUMENT, arg);

	/* Trigger command */
	uint16_t cmd_val = ((opcode & 0x3F) << 8) | (flags & 0xFF);
	sdhc_wr16(SDHCI_COMMAND, cmd_val);

	/* Poll for response or error (up to 100ms) */
	start = get_cntvct();
	ticks = 100000ULL * 192ULL / 10ULL;
	uint32_t is = 0;
	while (get_cntvct() - start < ticks) {
		is = sdhc_rd32(SDHCI_INT_STATUS);
		if (is & (SDHCI_INT_RESPONSE | SDHCI_INT_ERROR))
			break;
		__asm__ volatile("isb");
	}

	if (is & SDHCI_INT_ERROR) {
		ramoops_puts("  [SDHCI] CMD");
		ramoops_dec(opcode);
		ramoops_puts("(0x");
		ramoops_hex(arg, 8);
		ramoops_puts(") ERR: is=0x");
		ramoops_hex(is, 8);
		ramoops_puts("\n");
		sdhc_wr32(SDHCI_INT_STATUS, is);
		/* Reset CMD line on error so inhibit clears */
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD);
		uint64_t t = get_cntvct();
		while ((sdhc_rd8(SDHCI_SOFTWARE_RESET) & SDHCI_RESET_CMD) &&
		       (get_cntvct() - t < 10000ULL * 192ULL / 10ULL)) {
			__asm__ volatile("isb");
		}
		return -3;
	}
	if (!(is & SDHCI_INT_RESPONSE)) {
		ramoops_puts("  [SDHCI] CMD");
		ramoops_dec(opcode);
		ramoops_puts("(0x");
		ramoops_hex(arg, 8);
		ramoops_puts(") TOUT: is=0x");
		ramoops_hex(is, 8);
		ramoops_puts(" ps=0x");
		ramoops_hex(sdhc_rd32(SDHCI_PRESENT_STATE), 8);
		ramoops_puts("\n");
		/* Reset CMD line on timeout so inhibit clears */
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD);
		uint64_t t = get_cntvct();
		while ((sdhc_rd8(SDHCI_SOFTWARE_RESET) & SDHCI_RESET_CMD) &&
		       (get_cntvct() - t < 10000ULL * 192ULL / 10ULL)) {
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

	ramoops_puts("  [SDHCI] Resetting eMMC 5.0 card out of HS200 into Idle...\n");

	/* JEDEC eMMC 5.0: CMD0(0xF0F0F0F0) resets HS200/HS400 device to Pre-Idle */
	sdhc_send_cmd(0, 0xF0F0F0F0, SDHCI_CMD_RESP_NONE, 0);
	delay_us(5000);

	/* CMD0(0x00000000) resets card to Idle state */
	sdhc_send_cmd(0, 0x00000000, SDHCI_CMD_RESP_NONE, 0);
	delay_us(10000); /* 10 ms (at least 74 clock cycles) */

	/* CMD1: SEND_OP_COND probe with arg = 0 to read supported OCR */
	uint32_t ocr = 0;
	int ret = sdhc_send_cmd(1, 0x00000000, SDHCI_CMD_RESP_SHORT, &ocr);
	ramoops_puts("  CMD1 Probe Result : ret=");
	ramoops_dec(ret < 0 ? -ret : ret);
	ramoops_puts(", OCR=0x");
	ramoops_hex(ocr, 8);
	ramoops_puts("\n");

	/* If initial probe succeeded or failed, attempt full voltage negotiation */
	uint32_t req_ocr = 0x40FF8080; /* Sector mode | Dual voltage (1.8V/3.0V) */
	int ocr_ready = 0;
	for (int i = 0; i < 100; i++) {
		ret = sdhc_send_cmd(1, req_ocr, SDHCI_CMD_RESP_SHORT, &ocr);
		if (ret == 0 && (ocr & 0x80000000)) {
			ocr_ready = 1;
			break;
		}
		delay_us(10000); /* 10ms */
	}
	if (!ocr_ready) {
		ramoops_puts("  [SDHCI] CMD1 timed out waiting for OCR ready! OCR=0x");
		ramoops_hex(ocr, 8);
		ramoops_puts("\n");
		return -1;
	}
	ramoops_puts("  Card Ready OCR  : 0x");
	ramoops_hex(ocr, 8);
	if (ocr & 0x40000000)
		ramoops_puts(" (High Capacity / Sector Addressing)\n");
	else
		ramoops_puts(" (Standard Capacity / Byte Addressing)\n");

	/* CMD2: ALL_SEND_CID (R2 response: 136-bit) */
	ret = sdhc_send_cmd(2, 0, SDHCI_CMD_RESP_LONG | SDHCI_CMD_CRC_CHECK, 0);
	if (ret != 0) {
		ramoops_puts("  [SDHCI] CMD2 failed!\n");
		return -2;
	}
	ramoops_puts("  Card CID        : Read OK\n");

	/* CMD3: SET_RELATIVE_ADDR (Assign RCA = 1) */
	ret = sdhc_send_cmd(3, 0x00010000,
	    SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_RESP_SHORT,
	    &status);
	if (ret != 0) {
		ramoops_puts("  [SDHCI] CMD3 failed!\n");
		return -3;
	}
	ramoops_puts("  RCA Assigned    : 1 (Status: 0x");
	ramoops_hex(status, 8);
	ramoops_puts(")\n");

	/* CMD7: SELECT_CARD (RCA = 1, enters TRAN state) */
	ret = sdhc_send_cmd(7, 0x00010000,
	    SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_RESP_SHORT_BUSY,
	    &status);
	if (ret != 0) {
		ramoops_puts("  [SDHCI] CMD7 failed!\n");
		return -4;
	}
	ramoops_puts("  Card Selected   : State TRAN (Status: 0x");
	ramoops_hex(status, 8);
	ramoops_puts(")\n");

	/* CMD6: SWITCH to 8-bit bus width (EXT_CSD[183] = 2) */
	sdhc_send_cmd(6, 0x03B70200,
	    SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_RESP_SHORT_BUSY,
	    &status);
	sdhc_wr8(SDHCI_HOST_CONTROL, SDHCI_CTRL_8BITBUS);
	delay_us(1000);

	/* Switch clock to 25 MHz */
	sdhc_set_clock(25000000);

	/* CMD16: SET_BLOCKLEN to 512 */
	sdhc_send_cmd(16, 512,
	    SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_RESP_SHORT,
	    &status);

	/* Verify state with CMD13 */
	ret = sdhc_send_cmd(13, 0x00010000,
	    SDHCI_CMD_INDEX_CHECK | SDHCI_CMD_CRC_CHECK | SDHCI_CMD_RESP_SHORT,
	    &status);
	uint32_t final_state = (status >> 9) & 0xF;
	ramoops_puts("  Final Card State: ");
	ramoops_dec(final_state);
	if (final_state == 4)
		ramoops_puts(" (TRAN - Ready)\n");
	else
		ramoops_puts(" (Unexpected state)\n");

	return (final_state == 4) ? 0 : -5;
}

static int sdhc_read_single_block(uint32_t lba, uint8_t *buf)
{
	uint64_t start = get_cntvct();
	uint64_t ticks = 50000ULL * 192ULL / 10ULL;

	while (sdhc_rd32(SDHCI_PRESENT_STATE) & (SDHCI_CMD_INHIBIT | SDHCI_DATA_INHIBIT)) {
		if (get_cntvct() - start >= ticks) {
			ramoops_puts("  [SDHCI] read_block: inhibit timeout before CMD17\n");
			sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
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

	/* Wait for command response */
	start = get_cntvct();
	ticks = 100000ULL * 192ULL / 10ULL;
	uint32_t is = 0;
	while (get_cntvct() - start < ticks) {
		is = sdhc_rd32(SDHCI_INT_STATUS);
		if (is & (SDHCI_INT_RESPONSE | SDHCI_INT_ERROR))
			break;
		__asm__ volatile("isb");
	}
	if (is & SDHCI_INT_ERROR) {
		ramoops_puts("  [SDHCI] CMD17 response error! INT_STATUS=0x");
		ramoops_hex(is, 8);
		ramoops_puts("\n");
		sdhc_wr32(SDHCI_INT_STATUS, is);
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		return -2;
	}
	if (!(is & SDHCI_INT_RESPONSE)) {
		ramoops_puts("  [SDHCI] CMD17 response timeout!\n");
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		return -3;
	}
	sdhc_wr32(SDHCI_INT_STATUS, SDHCI_INT_RESPONSE);

	/* Wait for buffer data ready */
	start = get_cntvct();
	ticks = 200000ULL * 192ULL / 10ULL; /* 200ms */
	int data_ready = 0;
	while (get_cntvct() - start < ticks) {
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
		ramoops_puts("  [SDHCI] Buffer Read Ready timeout/err! is=0x");
		ramoops_hex(is, 8);
		ramoops_puts(" pstate=0x");
		ramoops_hex(sdhc_rd32(SDHCI_PRESENT_STATE), 8);
		ramoops_puts("\n");
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		return -4;
	}
	sdhc_wr32(SDHCI_INT_STATUS, SDHCI_INT_DATA_AVAIL);

	/* Read 128 32-bit words (512 bytes) from FIFO */
	uint32_t *p = (uint32_t *)buf;
	for (int i = 0; i < 128; i++) {
		p[i] = sdhc_rd32(SDHCI_BUFFER);
	}

	/* Wait for data transfer complete */
	start = get_cntvct();
	ticks = 100000ULL * 192ULL / 10ULL;
	while (get_cntvct() - start < ticks) {
		is = sdhc_rd32(SDHCI_INT_STATUS);
		if (is & (SDHCI_INT_DATA_END | SDHCI_INT_ERROR))
			break;
		__asm__ volatile("isb");
	}
	if (is & SDHCI_INT_ERROR) {
		ramoops_puts("  [SDHCI] Data End error! is=0x");
		ramoops_hex(is, 8);
		ramoops_puts("\n");
		sdhc_wr32(SDHCI_INT_STATUS, is);
		sdhc_wr8(SDHCI_SOFTWARE_RESET, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
		return -5;
	}
	sdhc_wr32(SDHCI_INT_STATUS, 0xFFFFFFFF);
	return 0;
}

int sdhci_probe(void)
{
	if (is_qemu) {
		ramoops_puts("\n[SDHCI] QEMU virt target detected; skipping MSM8994 SDCC probe.\n");
		return 0;
	}

	ramoops_puts("\n=======================================================\n");
	ramoops_puts("  [SDHCI] MSM8994 eMMC 5.0 Host Controller & GPT Probe\n");
	ramoops_puts("=======================================================\n");

	/* 1. Dump initial hardware MMIO state before touching anything */
	ramoops_puts("--- Initial Hardware MMIO State ---\n");
	ramoops_puts("  CORE_MCI_VER  : 0x"); ramoops_hex(sdhc_core_rd32(0x50), 8);
	ramoops_puts(" (SDCC major "); ramoops_dec((sdhc_core_rd32(0x50) >> 28) & 0xF); ramoops_puts(")\n");
	ramoops_puts("  CORE_POWER    : 0x"); ramoops_hex(sdhc_core_rd32(0x00), 8); ramoops_puts("\n");
	ramoops_puts("  CORE_HC_MODE  : 0x"); ramoops_hex(sdhc_core_rd32(0x78), 8); ramoops_puts("\n");
	ramoops_puts("  CORE_PWRCTL_ST: 0x"); ramoops_hex(sdhc_core_rd32(0xDC), 8); ramoops_puts("\n");
	ramoops_puts("  CORE_PWRCTL_CT: 0x"); ramoops_hex(sdhc_core_rd32(0xE8), 8); ramoops_puts("\n");
	ramoops_puts("  CORE_DLL_CFG  : 0x"); ramoops_hex(sdhc_rd32(0x100), 8); ramoops_puts("\n");
	ramoops_puts("  CORE_DLL_STS  : 0x"); ramoops_hex(sdhc_rd32(0x108), 8); ramoops_puts("\n");
	ramoops_puts("  CORE_VNDR_SPEC: 0x"); ramoops_hex(sdhc_rd32(0x10C), 8); ramoops_puts("\n");
	ramoops_puts("  CORE_VNDR_FN2 : 0x"); ramoops_hex(sdhc_rd32(0x110), 8); ramoops_puts("\n");
	ramoops_puts("  TLMM_SDC1_CTL : 0x"); ramoops_hex(*(volatile uint32_t *)TLMM_SDC1_CTL_REG, 8); ramoops_puts("\n");
	ramoops_puts("  SDHCI_PSTATE  : 0x"); ramoops_hex(sdhc_rd32(0x24), 8); ramoops_puts("\n");
	ramoops_puts("  SDHCI_HCTRL1/2: 0x"); ramoops_hex(sdhc_rd8(0x28), 2); ramoops_puts(" / 0x"); ramoops_hex(sdhc_rd16(0x3E), 4); ramoops_puts("\n");
	ramoops_puts("  SDHCI_CLK/PWR : 0x"); ramoops_hex(sdhc_rd16(0x2C), 4); ramoops_puts(" / 0x"); ramoops_hex(sdhc_rd8(0x29), 2); ramoops_puts("\n");

	/* 2. Reset Qualcomm core & transition to Host Controller mode */
	ramoops_puts("  [SDHCI] Performing Qualcomm Core Reset and HC Mode transition...\n");
	sdhc_core_wr32(CORE_HC_MODE, 0);
	__asm__ volatile("dmb sy" ::: "memory");

	sdhc_core_wr32(CORE_POWER, sdhc_core_rd32(CORE_POWER) | CORE_SW_RST);
	__asm__ volatile("dmb sy" ::: "memory");
	uint64_t rst_start = get_cntvct();
	while (sdhc_core_rd32(CORE_POWER) & CORE_SW_RST) {
		if (get_cntvct() - rst_start > 5000ULL * 192ULL / 10ULL)
			break;
		__asm__ volatile("isb");
	}

	sdhc_core_wr32(CORE_HC_MODE, HC_MODE_EN | FF_CLK_SW_RST_DIS);
	__asm__ volatile("dmb sy" ::: "memory");

	/* 3. Configure CORE_VENDOR_SPEC */
	uint32_t vndr = sdhc_rd32(CORE_VENDOR_SPEC);
	vndr &= ~CORE_CLK_PWRSAVE;                  /* Disable clock power save (auto gating) */
	vndr &= ~CORE_HC_MCLK_SEL_MASK;             /* Select default free-running MCLK */
	vndr |= CORE_HC_MCLK_SEL_DFLT;
	vndr &= ~(CORE_HC_SELECT_IN_EN | CORE_HC_SELECT_IN_MASK); /* Disable HC_SELECT_IN */
	vndr |= CORE_IO_PAD_PWR_SWITCH_EN;          /* Enable pad power switch */
	sdhc_wr32(CORE_VENDOR_SPEC, vndr);
	__asm__ volatile("dmb sy" ::: "memory");

	/* 4. Disable / Reset DLL for <= 100 MHz operations */
	uint32_t dll = sdhc_rd32(CORE_DLL_CONFIG);
	dll |= (CORE_DLL_RST | CORE_DLL_PDN);
	sdhc_wr32(CORE_DLL_CONFIG, dll);
	__asm__ volatile("dmb sy" ::: "memory");

	/* 5. Handle any pending power controller status */
	uint32_t pwrctl_sts = sdhc_core_rd32(CORE_PWRCTL_STATUS);
	if (pwrctl_sts) {
		sdhc_core_wr32(CORE_PWRCTL_CLEAR, pwrctl_sts);
		uint32_t ctl = sdhc_core_rd32(CORE_PWRCTL_CTL);
		if (pwrctl_sts & (CORE_PWRCTL_BUS_ON | CORE_PWRCTL_BUS_OFF))
			ctl |= CORE_PWRCTL_BUS_SUCCESS;
		if (pwrctl_sts & (CORE_PWRCTL_IO_HIGH | CORE_PWRCTL_IO_LOW))
			ctl |= CORE_PWRCTL_IO_SUCCESS;
		sdhc_core_wr32(CORE_PWRCTL_CTL, ctl);
		__asm__ volatile("dmb sy" ::: "memory");
	}

	/* 6. Setup SDHCI baseline registers */
	sdhc_wr8(SDHCI_HOST_CONTROL, 0);              /* 1-bit bus */
	sdhc_wr16(SDHCI_HOST_CONTROL2, 0);            /* Legacy mode / SDR12 */
	sdhc_wr8(SDHCI_TIMEOUT_CONTROL, 0x0E);         /* Max timeout */
	sdhc_wr8(SDHCI_POWER_CONTROL, 0x0B);           /* 1.8V VDD + SDHCI_POWER_ON */
	delay_us(10000);

	/* 7. Enable clock at 400 KHz */
	sdhc_set_clock(400000);

	/* 8. Enable status reporting in INT_STATUS, keep signal line masked */
	sdhc_wr32(SDHCI_INT_ENABLE, 0xFFFFFFFF);
	sdhc_wr32(SDHCI_SIGNAL_ENABLE, 0x00000000);

	/* 9. Print configured state */
	ramoops_puts("--- Active Configured MMIO State ---\n");
	ramoops_puts("  CORE_HC_MODE  : 0x"); ramoops_hex(sdhc_core_rd32(0x78), 8); ramoops_puts("\n");
	ramoops_puts("  CORE_DLL_CFG  : 0x"); ramoops_hex(sdhc_rd32(0x100), 8); ramoops_puts("\n");
	ramoops_puts("  CORE_VNDR_SPEC: 0x"); ramoops_hex(sdhc_rd32(0x10C), 8); ramoops_puts("\n");
	ramoops_puts("  SDHCI Host Ver: 0x"); ramoops_hex(sdhc_rd16(SDHCI_HOST_VERSION), 4); ramoops_puts("\n");
	ramoops_puts("  Present State : 0x"); ramoops_hex(sdhc_rd32(SDHCI_PRESENT_STATE), 8); ramoops_puts("\n");
	ramoops_puts("  Clock / Power : 0x"); ramoops_hex(sdhc_rd16(SDHCI_CLOCK_CONTROL), 4);
	ramoops_puts(" / 0x"); ramoops_hex(sdhc_rd8(SDHCI_POWER_CONTROL), 2); ramoops_puts("\n");

	/* Initialize / Handshake with eMMC card */
	int ret = sdhc_init_card();
	if (ret != 0) {
		ramoops_puts("  [SDHCI] Warning: Card initialization failed (code ");
		ramoops_dec(-ret);
		ramoops_puts(")\n");
	}

	/* Read LBA 0: Protective MBR */
	ramoops_puts("\n[SDHCI] Reading LBA 0 (Protective MBR)...\n");
	ret = sdhc_read_single_block(0, sec_buf);
	if (ret != 0) {
		ramoops_puts("  [SDHCI] Failed to read LBA 0!\n");
		return -2;
	}

	uint16_t mbr_sig = *(uint16_t *)&sec_buf[510];
	ramoops_puts("  MBR Signature   : 0x");
	ramoops_hex(mbr_sig, 4);
	if (mbr_sig == 0xAA55) {
		ramoops_puts(" (Valid 0x55AA)\n");
	} else {
		ramoops_puts(" (INVALID!)\n");
		return -3;
	}

	uint8_t part1_type = sec_buf[0x1BE + 4];
	ramoops_puts("  Partition 1 Type: 0x");
	ramoops_hex(part1_type, 2);
	if (part1_type == 0xEE) {
		ramoops_puts(" (EFI GPT Protective)\n");
	} else {
		ramoops_puts(" (Non-GPT MBR)\n");
	}

	/* Read LBA 1: Primary GPT Header */
	ramoops_puts("\n[SDHCI] Reading LBA 1 (Primary GPT Header)...\n");
	ret = sdhc_read_single_block(1, sec_buf);
	if (ret != 0) {
		ramoops_puts("  [SDHCI] Failed to read LBA 1!\n");
		return -4;
	}

	struct gpt_header *hdr = (struct gpt_header *)sec_buf;
	ramoops_puts("  GPT Signature   : 0x");
	ramoops_hex(hdr->signature, 16);
	if (hdr->signature == 0x5452415020494645ULL) {
		ramoops_puts(" (\"EFI PART\" - Valid)\n");
	} else {
		ramoops_puts(" (INVALID GPT!)\n");
		return -5;
	}

	ramoops_puts("  Disk First/Last : LBA ");
	ramoops_dec(hdr->first_usable_lba);
	ramoops_puts(" .. ");
	ramoops_dec(hdr->last_usable_lba);
	uint64_t total_mb = ((hdr->last_usable_lba + 34) * 512) / (1024 * 1024);
	ramoops_puts(" (Total: ");
	ramoops_dec(total_mb);
	ramoops_puts(" MB / ");
	ramoops_dec(total_mb / 1024);
	ramoops_puts(" GB)\n");

	uint64_t part_lba = hdr->partition_entry_lba;
	uint32_t num_parts = hdr->num_partition_entries;
	uint32_t part_sz = hdr->sizeof_partition_entry;

	ramoops_puts("  Partitions LBA  : ");
	ramoops_dec(part_lba);
	ramoops_puts(" (Count: ");
	ramoops_dec(num_parts);
	ramoops_puts(", Entry Size: ");
	ramoops_dec(part_sz);
	ramoops_puts(" bytes)\n\n");

	ramoops_puts("--- GPT Partition Table ---\n");

	if (num_parts > 48) num_parts = 48; /* Safety bound */
	uint64_t cur_sector = 0xFFFFFFFFFFFFFFFFULL;
	uint64_t userdata_start = 0;
	int userdata_found = 0;

	for (uint32_t i = 0; i < num_parts; i++) {
		uint64_t lba_for_entry = part_lba + (i * part_sz / 512);
		uint32_t offset_in_sector = (i * part_sz) % 512;

		if (lba_for_entry != cur_sector) {
			ret = sdhc_read_single_block((uint32_t)lba_for_entry, sec_buf);
			if (ret != 0) {
				ramoops_puts("  [SDHCI] Failed to read partition table sector ");
				ramoops_dec(lba_for_entry);
				ramoops_puts("!\n");
				break;
			}
			cur_sector = lba_for_entry;
		}

		struct gpt_entry *entry = (struct gpt_entry *)(sec_buf + offset_in_sector);

		/* Check if type GUID is all zeroes (unused) */
		int non_zero = 0;
		for (int g = 0; g < 16; g++) {
			if (entry->type_guid[g] != 0) {
				non_zero = 1;
				break;
			}
		}
		if (!non_zero) continue;

		/* Convert UTF-16LE partition name to ASCII */
		char name[37];
		for (int c = 0; c < 36; c++) {
			uint16_t wc = entry->partition_name[c];
			if (!wc) {
				name[c] = '\0';
				break;
			}
			name[c] = (wc < 128) ? (char)wc : '?';
		}
		name[36] = '\0';

		uint64_t start_lba = entry->starting_lba;
		uint64_t end_lba = entry->ending_lba;
		uint64_t blocks = end_lba - start_lba + 1;
		uint64_t mb = (blocks * 512) / (1024 * 1024);

		ramoops_puts("  [");
		ramoops_dec_pad(i + 1, 2);
		ramoops_puts("] ");
		ramoops_str_pad(name, 16);
		ramoops_puts(" start=");
		ramoops_dec_pad(start_lba, 10);
		ramoops_puts(" end=");
		ramoops_dec_pad(end_lba, 10);
		ramoops_puts(" blocks=");
		ramoops_dec_pad(blocks, 10);
		ramoops_puts(" (");
		ramoops_dec_pad(mb, 6);
		ramoops_puts(" MB)\n");

		/* Match userdata partition */
		if (name[0] == 'u' && name[1] == 's' && name[2] == 'e' &&
		    name[3] == 'r' && name[4] == 'd' && name[5] == 'a' &&
		    name[6] == 't' && name[7] == 'a' && name[8] == '\0') {
			userdata_start = start_lba;
			userdata_found = 1;
		}
	}

	if (userdata_found) {
		ramoops_puts("\n[SDHCI] Userdata partition identified at LBA ");
		ramoops_dec(userdata_start);
		ramoops_puts("!\n");

		/* Read superblock of userdata (offset 1024 bytes = 2 sectors from userdata_start) */
		ret = sdhc_read_single_block((uint32_t)(userdata_start + 2), sec_buf);
		if (ret == 0) {
			uint16_t fs_magic = *(uint16_t *)&sec_buf[0x38];
			ramoops_puts("  Userdata Superblock Magic: 0x");
			ramoops_hex(fs_magic, 4);
			if (fs_magic == 0xEF53) {
				ramoops_puts(" (ext4 filesystem detected at /data)\n");
				ramoops_puts("  [SDHCI] >>> Physical eMMC 5.0 sector read 100% verified! <<<\n");
			} else {
				ramoops_puts(" (Non-ext4 filesystem)\n");
			}
		}
	} else {
		ramoops_puts("  [SDHCI] Warning: 'userdata' partition entry not found!\n");
	}

	ramoops_puts("=======================================================\n\n");
	return 0;
}
