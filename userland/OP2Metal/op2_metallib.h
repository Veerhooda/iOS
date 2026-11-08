/*
 * OP2Metal: MTLB container parser for OnePlus 2 Metal shim.
 * Reads Apple's MTLB container format and extracts function names and AIR bitcode.
 */
#ifndef OP2_METALLIB_H
#define OP2_METALLIB_H

#include <stddef.h>
#include <stdint.h>

enum op2_metallib_type {
	OP2_METALLIB_VERTEX = 0,
	OP2_METALLIB_FRAGMENT = 1,
	OP2_METALLIB_KERNEL = 2,
	OP2_METALLIB_UNQUALIFIED = 3,
	OP2_METALLIB_VISIBLE = 4,
	OP2_METALLIB_EXTERN = 5,
	OP2_METALLIB_INTERSECTION = 6,
	OP2_METALLIB_NO_TYPE = 255,
};

struct op2_metallib_function {
	const char *name;
	uint32_t name_length;
	uint8_t type;
	uint16_t air[4];
	uint64_t module_offset;
	uint64_t module_size;
};

struct op2_metallib {
	uint16_t version[2];
	uint64_t file_size;
	uint32_t count;
};

int op2_metallib_parse(const uint8_t *data, size_t size, struct op2_metallib *info,
    struct op2_metallib_function *out, uint32_t max, const char **why);

int op2_metallib_thin(const uint8_t *data, size_t size, const uint8_t **thin, size_t *thin_size, const char **why);

const char *op2_metallib_type_name(uint8_t type);

int op2_metallib_bitcode(const uint8_t *data, size_t size, const struct op2_metallib_function *function,
    const uint8_t **bitcode, uint64_t *length);

#endif /* OP2_METALLIB_H */
