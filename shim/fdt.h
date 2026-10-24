#pragma once
#include <stdint.h>

void fdt_dump_all(uintptr_t fdt);
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);
int fdt_chosen_str(uintptr_t fdt, const char *prop, char *out, int max_len);
