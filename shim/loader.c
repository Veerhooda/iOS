/*
 * Bare-metal XNU / Darwin bootloader for OnePlus 2 (MSM8994).
 *
 * Receives the FDT pointer from Little Kernel, locates the OP2XNU01 payload
 * in the ramdisk, maps segments into high-bank DRAM (0x10200000+),
 * lays down the Apple Device Tree and xnu-4903 boot_args, and transfers control.
 */
#include <stdint.h>
#include "fdt.h"
#include "loader.h"

#define BOOT_LINE_LENGTH	256
#define PAGE_4K			0x1000UL

void ramoops_puts(const char *s);
void ramoops_putc(char c);
void ramoops_hex(uint64_t val, int digits);

struct payload_seg {
	uint64_t vmaddr;
	uint64_t vmsize;
	uint64_t file_off;
	uint64_t file_size;
};

struct payload {
	char magic[8];
	uint32_t version;
	uint32_t nsegs;
	uint64_t virt_base;
	uint64_t entry_va;
	uint64_t adt_off;
	uint64_t adt_size;
	uint64_t kernel_end;
	uint64_t phys_base;
	uint64_t mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h for xnu-4903 */
struct boot_video {
	uint64_t v_baseAddr;
	uint64_t v_display;
	uint64_t v_rowBytes;
	uint64_t v_width;
	uint64_t v_height;
	uint64_t v_depth;
};

struct boot_args {
	uint16_t Revision;
	uint16_t Version;
	uint32_t _pad0;
	uint64_t virtBase;
	uint64_t physBase;
	uint64_t memSize;
	uint64_t topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint32_t _pad1;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint32_t _pad2;
	uint64_t bootFlags;
	uint64_t memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x148, "payload header offset");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args Video offset");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args CommandLine offset");
_Static_assert(sizeof(struct boot_args) == 384, "boot_args size must match xnu-4903 (384 bytes)");

/* Copy in 8-byte words */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	uint64_t words = (n + 7) / 8;
	for (uint64_t i = 0; i < words; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	uint64_t words = n / 8;
	for (uint64_t i = 0; i < words; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "OP2XNU01";
	for (int i = 0; i < 8; i++) {
		if (m[i] != want[i])
			return 0;
	}
	return 1;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

const char *loader_boot(uintptr_t fdt_addr)
{
	uint64_t rd_start = 0, rd_end = 0;

	if (!fdt_chosen_u64(fdt_addr, "linux,initrd-start", &rd_start) ||
	    !fdt_chosen_u64(fdt_addr, "linux,initrd-end", &rd_end)) {
		return "no initrd found in FDT /chosen";
	}

	ramoops_puts("[LOADER] Found ramdisk at 0x");
	ramoops_hex(rd_start, 16);
	ramoops_puts(" .. 0x");
	ramoops_hex(rd_end, 16);
	ramoops_puts(" (");
	ramoops_hex(rd_end - rd_start, 8);
	ramoops_puts(" bytes)\n");

	const struct payload *p = (const struct payload *)rd_start;
	if (!magic_ok(p->magic)) {
		ramoops_puts("[LOADER] Payload magic mismatch: got '");
		for (int i = 0; i < 8; i++)
			ramoops_putc(p->magic[i]);
		ramoops_puts("'\n");
		return "invalid payload magic";
	}

	if (p->version != 1)
		return "unsupported payload version";

	ramoops_puts("[LOADER] Valid OP2XNU01 payload detected!\n");
	ramoops_puts("  PhysBase   : 0x"); ramoops_hex(p->phys_base, 16); ramoops_puts("\n");
	ramoops_puts("  MemSize    : 0x"); ramoops_hex(p->mem_size, 16);  ramoops_puts("\n");
	ramoops_puts("  VirtBase   : 0x"); ramoops_hex(p->virt_base, 16); ramoops_puts("\n");
	ramoops_puts("  EntryVA    : 0x"); ramoops_hex(p->entry_va, 16);  ramoops_puts("\n");
	ramoops_puts("  Segments   : ");   ramoops_hex(p->nsegs, 2);      ramoops_puts("\n");

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;

	/* Verify no overlap between ramdisk and kernel destination */
	uint64_t kernel_load_end = p->phys_base + (p->kernel_end - p->virt_base) + 0x200000;
	if (rd_end > p->phys_base && rd_start < kernel_load_end)
		return "ramdisk overlaps kernel image destination";

	/* 1. Copy segments */
	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		ramoops_puts("  [LOADER] Seg ");
		ramoops_hex(i, 2);
		ramoops_puts(": dst=0x");
		ramoops_hex(dst, 16);
		ramoops_puts(" sz=0x");
		ramoops_hex(s->file_size, 8);
		ramoops_puts("\n");
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size) {
			uint64_t padded_file = (s->file_size + 7) & ~7UL;
			if (s->vmsize > padded_file)
				zero64(dst + padded_file, s->vmsize - padded_file);
		}
	}
	ramoops_puts("[LOADER] Segments relocated successfully.\n");

	/* 2. Copy Apple Device Tree behind kernel */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, rd_start + p->adt_off, p->adt_size);
	ramoops_puts("[LOADER] ADT placed at PA 0x");
	ramoops_hex(adt_pa, 16);
	ramoops_puts(" (");
	ramoops_hex(p->adt_size, 6);
	ramoops_puts(" bytes)\n");

	/* 3. Construct boot_args (4 KB page aligned) */
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_4K - 1) & ~(PAGE_4K - 1);
	uintptr_t top = ba_pa + PAGE_4K;
	zero64(ba_pa, PAGE_4K);

	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
	ba->memSize = p->mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = 0xac0be000UL; /* Ramoops console */
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = 4352;
	ba->Video.v_width = 1080;
	ba->Video.v_height = 1920;
	ba->Video.v_depth = 32;
	ba->machineType = 0;
	ba->deviceTreeP = adt_pa - pa_of_va_base; /* VA when MMU is on */
	ba->deviceTreeLength = (uint32_t)p->adt_size;

	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];

	ba->bootFlags = 0;
	ba->memSizeActual = p->mem_size;

	ramoops_puts("[LOADER] boot_args placed at PA 0x");
	ramoops_hex(ba_pa, 16);
	ramoops_puts(", topOfKernelData = 0x");
	ramoops_hex(top, 16);
	ramoops_puts("\n");
	ramoops_puts("  Cmdline: ");
	ramoops_puts((const char *)ba->CommandLine);
	ramoops_puts("\n");

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	ramoops_puts("[LOADER] Kernel Entry Point PA: 0x");
	ramoops_hex(entry_pa, 16);
	ramoops_puts("\n");
	ramoops_puts("[LOADER] Flushing CPU caches and transferring control to Darwin/XNU...\n\n");

	/* Invalidate icache and synchronize pipeline */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");

	/* Transfer control to XNU with boot_args in x0 */
	((xnu_entry_t)entry_pa)(ba_pa);

	return "XNU entry returned unexpectedly";
}
