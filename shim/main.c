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

static void ramoops_putc(char c)
{
	volatile struct persistent_ram_buffer *buf = (volatile void *)RAMOOPS_CONSOLE;
	uint32_t sz = buf->size;
	if (sz < 260000) {
		buf->data[sz] = c;
		buf->start = sz + 1;
		buf->size = sz + 1;
	}
}

static void ramoops_puts(const char *s)
{
	while (*s)
		ramoops_putc(*s++);
}

static void ramoops_hex(uint64_t v, int digits)
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
	ramoops_puts("\n[SHIM] Resetting SoC via PS_HOLD (0xfc4ab000)...\n");
	*(volatile uint32_t *)0xfc4ab000UL = 0;
	for (;;)
		__asm__ volatile("wfe");
}

void shim_main(uint64_t fdt_addr)
{
	/* 1. Initialize Ramoops console */
	ramoops_init();
	ramoops_puts("\n=======================================================\n");
	ramoops_puts("  [ONEPLUS2-SHIM] M1 On-Screen Text Console Active!\n");
	ramoops_puts("=======================================================\n");

	uint32_t el = get_current_el();
	ramoops_puts("  CurrentEL: EL");
	ramoops_putc('0' + el);
	ramoops_puts(", FDT: 0x");
	ramoops_hex(fdt_addr, 16);
	ramoops_puts("\n");

	/* 2. Clear Framebuffer to dark navy blue */
	con_clear(0x000a1a3a);

	/* 3. Render Header */
	con_str("TARGET", "OnePlus 2 (MSM8994 / ONE A2003)");
	con_str("PROJECT", "Darwin/XNU & iOS Porting Shim");
	con_str("STATUS", "M1 On-Screen Diagnostic Console");
	con_skip();

	/* 4. CPU & Firmware Details */
	char el_str[] = "EL0";
	el_str[2] = '0' + el;
	con_str("EXEC EL", el_str);
	con_hex("FDT ADDR", fdt_addr);

	uint32_t fdt_magic = *(volatile uint32_t *)fdt_addr;
	con_hex32("FDT MAGC", __builtin_bswap32(fdt_magic));

	/* 5. Parse FDT /chosen */
	uint64_t rd_start = 0, rd_end = 0;
	if (fdt_chosen_u64(fdt_addr, "linux,initrd-start", &rd_start)) {
		con_hex("RD START", rd_start);
		ramoops_puts("  Initrd start: 0x");
		ramoops_hex(rd_start, 16);
		ramoops_puts("\n");
	}
	if (fdt_chosen_u64(fdt_addr, "linux,initrd-end", &rd_end)) {
		con_hex("RD END  ", rd_end);
		ramoops_puts("  Initrd end:   0x");
		ramoops_hex(rd_end, 16);
		ramoops_puts("\n");
	}

	char cmdline[128];
	if (fdt_chosen_str(fdt_addr, "bootargs", cmdline, sizeof(cmdline))) {
		con_str("CMDLINE", cmdline);
		ramoops_puts("  Bootargs: ");
		ramoops_puts(cmdline);
		ramoops_puts("\n");
	}

	con_skip();
	con_str("DISPLAY", "1080x1920 @ 32bpp, Stride 4352");
	con_str("STACK", "0x00400000 (Safe Bare-Metal)");
	con_str("RAMOOPS", "0xac0be000 (256 KB Persistent)");
	con_skip();

	/* 6. Live 20-second Countdown */
	int count_row = con_get_row();
	char count_msg[] = "Rebooting into Android in 20s... ";
	for (int i = 20; i >= 0; i--) {
		con_set_row(count_row);
		count_msg[26] = '0' + (i / 10);
		count_msg[27] = '0' + (i % 10);
		con_str("COUNTDWN", count_msg);

		ramoops_puts("  Countdown: ");
		ramoops_puts(count_msg);
		ramoops_puts("\n");

		delay_sec(1);
	}

	/* 7. Reset */
	con_str("RESET", "Triggering PMIC reset via PS_HOLD...");
	delay_sec(1);
	reset_soc();
}
