/*
 * OnePlus 2 (MSM8994) M1 Diagnostic Console Main
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"

#define RAMOOPS_CONSOLE	0xac0be000UL
#define RAMOOPS_SIG	0x43474244U

void *memcpy(void *dest, const void *src, unsigned long n)
{
	char *d = dest;
	const char *s = src;
	while (n--)
		*d++ = *s++;
	return dest;
}

void *memset(void *s, int c, unsigned long n)
{
	char *p = s;
	while (n--)
		*p++ = (char)c;
	return s;
}

struct persistent_ram_buffer {
	uint32_t sig;
	uint32_t start;
	uint32_t size;
	char data[];
};

static void ramoops_init(void)
{
	volatile struct persistent_ram_buffer *buf = (volatile void *)RAMOOPS_CONSOLE;
	buf->sig = RAMOOPS_SIG;
	buf->start = 0;
	buf->size = 0;
}

void ramoops_putc(char c)
{
	volatile struct persistent_ram_buffer *buf = (volatile void *)RAMOOPS_CONSOLE;
	uint32_t sz = buf->size;
	if (sz < 260000) {
		buf->data[sz] = c;
		buf->start = sz + 1;
		buf->size = sz + 1;
	}
}

void ramoops_puts(const char *s)
{
	while (*s)
		ramoops_putc(*s++);
}

void ramoops_hex(uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		ramoops_putc(hex[(v >> (4 * (digits - 1 - i))) & 0xf]);
}

static inline uint32_t get_current_el(void)
{
	uint64_t el;
	__asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
	return (uint32_t)(el >> 2);
}

static inline uint64_t get_cntvct(void)
{
	uint64_t val;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(val));
	return val;
}

static void delay_sec(uint32_t sec)
{
	uint64_t start = get_cntvct();
	uint64_t ticks = (uint64_t)sec * 19200000ULL;
	while (get_cntvct() - start < ticks) {
		__asm__ volatile("isb");
	}
}

static void reset_soc(void)
{
	ramoops_puts("\n[SHIM] Setting restart_reason to RECOVERY (0x77665502)...\n");
	/* Write recovery reboot cookie to IMEM restart_reason (0xfe87f65c) */
	*(volatile uint32_t *)0xfe87f65cUL = 0x77665502;

	ramoops_puts("[SHIM] Deasserting PS_HOLD (0xfc4ab000) for PMIC reset...\n");
	*(volatile uint32_t *)0xfc4ab000UL = 0;
	for (;;)
		__asm__ volatile("wfe");
}

void shim_main(uint64_t fdt_addr, uint64_t entry_pc)
{
	/* 1. Initialize Ramoops console */
	ramoops_init();
	ramoops_puts("\n=======================================================\n");
	ramoops_puts("  [ONEPLUS2-SHIM] M1 Bare-Metal Diagnostics & FDT Audit\n");
	ramoops_puts("=======================================================\n");

	uint32_t el = get_current_el();
	ramoops_puts("  Actual Entry PC : 0x");
	ramoops_hex(entry_pc, 16);
	ramoops_puts("\n  CurrentEL       : EL");
	ramoops_putc('0' + el);
	ramoops_puts("\n  FDT Pointer (x0): 0x");
	ramoops_hex(fdt_addr, 16);
	ramoops_puts("\n");

	uint32_t fdt_magic = *(volatile uint32_t *)fdt_addr;
	ramoops_puts("  FDT Header Magic: 0x");
	ramoops_hex(__builtin_bswap32(fdt_magic), 8);
	ramoops_puts("\n");

	/* 2. Parse FDT /chosen */
	uint64_t rd_start = 0, rd_end = 0;
	if (fdt_chosen_u64(fdt_addr, "linux,initrd-start", &rd_start)) {
		ramoops_puts("  Initrd Start    : 0x");
		ramoops_hex(rd_start, 16);
		ramoops_puts("\n");
	}
	if (fdt_chosen_u64(fdt_addr, "linux,initrd-end", &rd_end)) {
		ramoops_puts("  Initrd End      : 0x");
		ramoops_hex(rd_end, 16);
		ramoops_puts(" (Length: ");
		ramoops_hex(rd_end - rd_start, 8);
		ramoops_puts(" bytes)\n");
	}

	char cmdline[256];
	if (fdt_chosen_str(fdt_addr, "bootargs", cmdline, sizeof(cmdline))) {
		ramoops_puts("  Bootargs        : ");
		ramoops_puts(cmdline);
		ramoops_puts("\n");
	}

	/* 3. Comprehensive FDT Dump (/memory, /reserved-memory, /cpus) */
	fdt_dump_all(fdt_addr);

	/* 4. Quick 2-second delay before rebooting directly into TWRP */
	ramoops_puts("\n[SHIM] Fast-loop rebooting directly into TWRP Recovery...\n");
	delay_sec(2);
	reset_soc();
}
