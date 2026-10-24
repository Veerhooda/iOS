/*
 * Flattened Device Tree parser and dumper for OnePlus 2 boot shim.
 * Traverses big-endian FDT tokens on bare-metal ARM64.
 */
#include "fdt.h"

#define FDT_MAGIC	0xd00dfeed
#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9

/* External ramoops print helpers */
void ramoops_puts(const char *s);
void ramoops_putc(char c);
void ramoops_hex(uint64_t v, int digits);

static inline uint32_t be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

static inline uint64_t be64(uintptr_t a)
{
	return ((uint64_t)be32(a) << 32) | be32(a + 4);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static int strstarts(const char *str, const char *prefix)
{
	while (*prefix) {
		if (*str++ != *prefix++)
			return 0;
	}
	return 1;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

void fdt_dump_all(uintptr_t fdt)
{
	if (!fdt || be32(fdt) != FDT_MAGIC) {
		ramoops_puts("[FDT] Invalid FDT magic!\n");
		return;
	}

	uintptr_t p = fdt + be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + be32(fdt + 12);	/* off_dt_strings */

	char current_node[64] = "/";
	char parent_node[64] = "";
	int depth = 0;

	ramoops_puts("\n--- [LIVE FDT DUMP FROM LITTLE KERNEL] ---\n");

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			if (depth == 1) {
				current_node[0] = '/';
				current_node[1] = 0;
			} else if (depth == 2) {
				int i = 0;
				while (name[i] && i < 60) {
					current_node[i] = name[i];
					i++;
				}
				current_node[i] = 0;
				parent_node[0] = 0;
			} else if (depth == 3) {
				int i = 0;
				while (current_node[i] && i < 30) {
					parent_node[i] = current_node[i];
					i++;
				}
				parent_node[i] = 0;
				int j = 0;
				while (name[j] && j < 30) {
					current_node[j] = name[j];
					j++;
				}
				current_node[j] = 0;
			}
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			depth--;
			if (depth < 0)
				return;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p);
			uint32_t nameoff = be32(p + 4);
			const char *pname = (const char *)(strings + nameoff);
			uintptr_t val = p + 8;

			/* Dump /memory node */
			if (strstarts(current_node, "memory") && streq(pname, "reg")) {
				ramoops_puts("  Node: /memory\n");
				ramoops_puts("    reg (length = ");
				ramoops_hex(len, 2);
				ramoops_puts(" bytes):\n");
				for (uint32_t off = 0; off + 16 <= len; off += 16) {
					uint64_t base = be64(val + off);
					uint64_t size = be64(val + off + 8);
					ramoops_puts("      base: 0x");
					ramoops_hex(base, 16);
					ramoops_puts("  size: 0x");
					ramoops_hex(size, 16);
					ramoops_puts(" (");
					ramoops_hex(size >> 20, 4);
					ramoops_puts(" MB)\n");
				}
			}

			/* Dump /reserved-memory children */
			if (strstarts(parent_node, "reserved-memory") && streq(pname, "reg")) {
				ramoops_puts("  Reserved-Mem: ");
				ramoops_puts(current_node);
				ramoops_puts("\n");
				for (uint32_t off = 0; off + 16 <= len; off += 16) {
					uint64_t base = be64(val + off);
					uint64_t size = be64(val + off + 8);
					ramoops_puts("      base: 0x");
					ramoops_hex(base, 16);
					ramoops_puts("  size: 0x");
					ramoops_hex(size, 16);
					ramoops_puts(" (");
					ramoops_hex(size >> 20, 4);
					ramoops_puts(" MB)\n");
				}
			}

			/* Dump /cpus/cpu@* */
			if (strstarts(parent_node, "cpus") && (streq(pname, "enable-method") || streq(pname, "compatible") || streq(pname, "device_type"))) {
				ramoops_puts("  CPU [");
				ramoops_puts(current_node);
				ramoops_puts("] ");
				ramoops_puts(pname);
				ramoops_puts(" = ");
				ramoops_puts((const char *)val);
				ramoops_puts("\n");
			}

			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:
			ramoops_puts("--- [END OF FDT] ---\n");
			return;
		}
	}
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	if (!fdt || be32(fdt) != FDT_MAGIC)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t strings = fdt + be32(fdt + 12);
	int depth = 0, in_chosen = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = (depth == 2 && streq(name, "chosen"));
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				if (len == 4)
					*out = be32(val);
				else if (len == 8)
					*out = ((uint64_t)be32(val) << 32) | be32(val + 4);
				else
					return 0;
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:
			return 0;
		}
	}
}

int fdt_chosen_str(uintptr_t fdt, const char *prop, char *out, int max_len)
{
	if (!fdt || be32(fdt) != FDT_MAGIC || max_len <= 0)
		return 0;
	uintptr_t p = fdt + be32(fdt + 8);
	uintptr_t strings = fdt + be32(fdt + 12);
	int depth = 0, in_chosen = 0;

	for (;;) {
		uint32_t tok = be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			in_chosen = (depth == 2 && streq(name, "chosen"));
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (in_chosen)
				return 0;
			depth--;
			break;
		case FDT_PROP: {
			uint32_t len = be32(p), nameoff = be32(p + 4);
			uintptr_t val = p + 8;
			if (in_chosen && streq((const char *)(strings + nameoff), prop)) {
				int c = 0;
				while (c < (int)len && c < max_len - 1) {
					out[c] = *(volatile char *)(val + c);
					if (out[c] == 0)
						break;
					c++;
				}
				out[c] = 0;
				return 1;
			}
			p = align4(val + len);
			break;
		}
		case FDT_NOP:
			break;
		default:
			return 0;
		}
	}
}
