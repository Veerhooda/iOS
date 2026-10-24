#pragma once
#include <stdint.h>

#define FB_BASE		0x03401000UL	/* cont_splash_mem on MSM8994 OnePlus 2 */
#define FB_W		1080
#define FB_H		1920
#define FB_STRIDE	4352		/* 1088 * 4 bytes per row */

#define BLACK		0x00000000U
#define DARK_BLUE	0x00000040U
#define RED		0x00ff0000U
#define GREEN		0x0000ff00U
#define BLUE		0x000000ffU
#define WHITE		0x00ffffffU
#define YELLOW		0x00ffff00U
#define CYAN		0x0000ffffU
#define MAGENTA		0x00ff00ffU

extern volatile uint32_t *con_fb;

void con_clear(uint32_t bg);
void con_skip(void);
int con_get_row(void);
void con_set_row(int r);
void con_str(const char *label, const char *s);
void con_hex(const char *label, uint64_t v);
void con_hex32(const char *label, uint32_t v);
void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb);
