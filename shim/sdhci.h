/*
 * OnePlus 2 (MSM8994) eMMC 5.0 / SDHCI Bare-Metal Driver
 * JEDEC eMMC Standard / SD Host Controller Standard Specification
 */
#pragma once
#include <stdint.h>

extern int is_qemu;

void ramoops_putc(char c);
void ramoops_puts(const char *s);
void ramoops_hex(uint64_t v, int digits);

/* Probe SDHCI host controller 1 and read partition table */
int sdhci_probe(void);
