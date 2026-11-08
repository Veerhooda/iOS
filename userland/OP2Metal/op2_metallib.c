/*
 * OP2Metal: MTLB container parser implementation for OnePlus 2.
 */
#include "op2_metallib.h"
#include <string.h>

#define HEADER_SIZE     0x58

static uint16_t
u16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static uint32_t
u32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint64_t
u64(const uint8_t *p)
{
	return (uint64_t)u32(p) | (uint64_t)u32(p + 4) << 32;
}

static int
inside(uint64_t offset, uint64_t length, uint64_t size)
{
	return offset <= size && length <= size - offset;
}

int
op2_metallib_parse(const uint8_t *data, size_t size, struct op2_metallib *info,
    struct op2_metallib_function *out, uint32_t max, const char **why)
{
	uint64_t list, list_length, list_end, modules, modules_length, p;
	uint32_t count;

	if (size < HEADER_SIZE || memcmp(data, "MTLB", 4) != 0) {
		*why = "not a metallib (no MTLB at the start, or shorter than header)";
		return -1;
	}
	info->version[0] = u16(data + 4);
	info->version[1] = u16(data + 6);
	info->file_size = u64(data + 0x10);
	list = u64(data + 0x18);
	list_length = u64(data + 0x20);
	modules = u64(data + 0x48);
	modules_length = u64(data + 0x50);
	if (!inside(list, 4, size) || !inside(list + 4, list_length, size)) {
		*why = "function list is not inside the file";
		return -1;
	}
	if (!inside(modules, modules_length, size)) {
		*why = "modules are not inside the file";
		return -1;
	}
	list_end = list + 4 + list_length;
	count = u32(data + list);
	info->count = count;
	p = list + 4;
	for (uint32_t i = 0; i < count; i++) {
		struct op2_metallib_function f = { .type = OP2_METALLIB_NO_TYPE };
		uint64_t entry, end, q;
		int have_offset = 0, have_size = 0;

		if (!inside(p, 4, list_end)) {
			*why = "function list ends prematurely";
			return -1;
		}
		entry = u32(data + p);
		if (entry < 4 || !inside(p, entry, list_end)) {
			*why = "function entry is not inside the function list";
			return -1;
		}
		end = p + entry;
		for (q = p + 4; q + 4 <= end && memcmp(data + q, "ENDT", 4) != 0;) {
			const uint8_t *value;
			uint16_t length;

			if (!inside(q, 6, end)) {
				*why = "tag runs past the end of function entry";
				return -1;
			}
			length = u16(data + q + 4);
			if (!inside(q + 6, length, end)) {
				*why = "tag value runs past the end of function entry";
				return -1;
			}
			value = data + q + 6;
			if (memcmp(data + q, "NAME", 4) == 0) {
				f.name = (const char *)value;
				f.name_length = length;
				while (f.name_length > 0 && f.name[f.name_length - 1] == '\0') {
					f.name_length--;
				}
			} else if (memcmp(data + q, "TYPE", 4) == 0 && length >= 1) {
				f.type = value[0];
			} else if (memcmp(data + q, "VERS", 4) == 0 && length >= 8) {
				for (int k = 0; k < 4; k++) {
					f.air[k] = u16(value + 2 * k);
				}
			} else if (memcmp(data + q, "OFFT", 4) == 0 && length >= 24) {
				f.module_offset = u64(value + 16);
				have_offset = 1;
			} else if (memcmp(data + q, "MDSZ", 4) == 0 && length >= 8) {
				f.module_size = u64(value);
				have_size = 1;
			}
			q += 6 + (uint64_t)length;
		}
		if (f.name == NULL || !have_offset || !have_size) {
			*why = "function missing NAME, OFFT, or MDSZ";
			return -1;
		}
		if (!inside(f.module_offset, f.module_size, modules_length)) {
			*why = "function module is outside modules segment";
			return -1;
		}
		f.module_offset += modules;
		if (i < max) {
			out[i] = f;
		}
		p = end;
	}
	return 0;
}

#define FAT_MAGIC       0xcafebabeu
#define CPU_TYPE_AIR64  0x01000017u

static uint32_t
be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

int
op2_metallib_thin(const uint8_t *data, size_t size, const uint8_t **thin, size_t *thin_size, const char **why)
{
	uint32_t count;

	if (size < 8 || be32(data) != FAT_MAGIC) {
		*thin = data;
		*thin_size = size;
		return 0;
	}
	count = be32(data + 4);
	for (uint32_t i = 0; i < count && i < 64; i++) {
		uint64_t at = 8 + 20 * (uint64_t)i;

		if (!inside(at, 20, size)) {
			break;
		}
		if (be32(data + at) == CPU_TYPE_AIR64) {
			uint32_t offset = be32(data + at + 8), length = be32(data + at + 12);

			if (!inside(offset, length, size)) {
				*why = "universal metallib AIR64 slice is out of bounds";
				return -1;
			}
			*thin = data + offset;
			*thin_size = length;
			return 0;
		}
	}
	*why = "universal metallib with no AIR64 slice";
	return -1;
}

const char *
op2_metallib_type_name(uint8_t type)
{
	static const char *const names[] = { "vertex", "fragment", "kernel", "unqualified", "visible", "extern", "intersection" };

	return type < sizeof(names) / sizeof(names[0]) ? names[type] : "?";
}

int
op2_metallib_bitcode(const uint8_t *data, size_t size, const struct op2_metallib_function *function,
    const uint8_t **bitcode, uint64_t *length)
{
	const uint8_t *module = data + function->module_offset;
	uint32_t offset, wrapped;

	if (!inside(function->module_offset, function->module_size, size)) {
		return -1;
	}
	if (function->module_size >= 16 && u32(module) == 0x0B17C0DEu) {
		offset = u32(module + 8);
		wrapped = u32(module + 12);
		if (!inside(offset, wrapped, function->module_size)) {
			return -1;
		}
		*bitcode = module + offset;
		*length = wrapped;
		return 0;
	}
	*bitcode = module;
	*length = function->module_size;
	return 0;
}
