#pragma once
#include <stdint.h>

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);
int fdt_chosen_str(uintptr_t fdt, const char *prop, char *out, int max_len);
int fdt_memory_range(uintptr_t fdt, uint64_t *base, uint64_t *size);
