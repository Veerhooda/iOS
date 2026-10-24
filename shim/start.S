/*
 * OnePlus 2 (MSM8994) M1 Bare-Metal Boot Shim
 *
 * Little Kernel (aboot) enters at EL1 with MMU off, D-cache off.
 * Registers on entry:
 *   x0 = Physical address of patched device tree (FDT) in RAM
 *   x1 = 0, x2 = 0, x3 = 0
 *
 * Memory map:
 *   0x00008000: Shim entry (_head)
 *   0x00009000: Appended DTB start (offset 0x1000 = 4096)
 *   0x00400000: Initial safe stack (grows down towards 0x00300000)
 *   0x03401000: Splash memory (framebuffer, 1080x1920)
 *   0xac0be000: Ramoops console zone (persistent_ram_buffer)
 *   0xfc4ab000: PS_HOLD register (write 0 to reset)
 */

	.section .text.head, "ax"
	.globl _head
_head:
	b	_start			/* 0x00: code0 */
	.long	0			/* 0x04: code1 */
	.quad	0x80000			/* 0x08: text_offset */
	.quad	0			/* 0x10: image_size (0 in Linux v0) */
	.quad	0			/* 0x18: flags */
	.quad	0			/* 0x20: res2 */
	.long	0			/* 0x28: res3 */
	.long	0x1000			/* 0x2C: DTB offset for Little Kernel (4096 bytes) */
	.quad	0			/* 0x30: res4 */
	.ascii	"ARM\x64"		/* 0x38: magic (0x644d5241) */
	.long	0			/* 0x3C: res5 */

/*
 * Exception vector table (must be aligned to 2048 bytes = 0x800)
 */
	.balign 0x800
	.globl vectors
vectors:
	/* Current EL with SP0 */
	b	exc_sync_sp0
	.balign 0x80
	b	exc_irq_sp0
	.balign 0x80
	b	exc_fiq_sp0
	.balign 0x80
	b	exc_serror_sp0
	.balign 0x80

	/* Current EL with SPx */
	b	exc_sync_spx
	.balign 0x80
	b	exc_irq_spx
	.balign 0x80
	b	exc_fiq_spx
	.balign 0x80
	b	exc_serror_spx
	.balign 0x80

	/* Lower EL using AArch64 */
	b	exc_sync_lower64
	.balign 0x80
	b	exc_irq_lower64
	.balign 0x80
	b	exc_fiq_lower64
	.balign 0x80
	b	exc_serror_lower64
	.balign 0x80

	/* Lower EL using AArch32 */
	b	exc_sync_lower32
	.balign 0x80
	b	exc_irq_lower32
	.balign 0x80
	b	exc_fiq_lower32
	.balign 0x80
	b	exc_serror_lower32
	.balign 0x80

/* Exception handlers */
exc_sync_sp0:
	mov	x17, #0
	b	handle_exception
exc_irq_sp0:
	mov	x17, #1
	b	handle_exception
exc_fiq_sp0:
	mov	x17, #2
	b	handle_exception
exc_serror_sp0:
	mov	x17, #3
	b	handle_exception

exc_sync_spx:
	mov	x17, #4
	b	handle_exception
exc_irq_spx:
	mov	x17, #5
	b	handle_exception
exc_fiq_spx:
	mov	x17, #6
	b	handle_exception
exc_serror_spx:
	mov	x17, #7
	b	handle_exception

exc_sync_lower64:
	mov	x17, #8
	b	handle_exception
exc_irq_lower64:
	mov	x17, #9
	b	handle_exception
exc_fiq_lower64:
	mov	x17, #10
	b	handle_exception
exc_serror_lower64:
	mov	x17, #11
	b	handle_exception

exc_sync_lower32:
	mov	x17, #12
	b	handle_exception
exc_irq_lower32:
	mov	x17, #13
	b	handle_exception
exc_fiq_lower32:
	mov	x17, #14
	b	handle_exception
exc_serror_lower32:
	mov	x17, #15
	b	handle_exception

handle_exception:
	/* Save exception registers into x14, x15, x16 */
	mrs	x14, esr_el1
	mrs	x15, far_el1
	mrs	x16, elr_el1

	/* Append exception info to ramoops buffer */
	adr	x0, str_exc_header
	bl	ramoops_puts

	/* Print slot number */
	mov	x0, x17
	bl	ramoops_puthex8

	adr	x0, str_exc_esr
	bl	ramoops_puts
	mov	x0, x14
	bl	ramoops_puthex64

	adr	x0, str_exc_far
	bl	ramoops_puts
	mov	x0, x15
	bl	ramoops_puthex64

	adr	x0, str_exc_elr
	bl	ramoops_puts
	mov	x0, x16
	bl	ramoops_puthex64

	adr	x0, str_exc_newline
	bl	ramoops_puts

	/* Flash screen solid RED on panic */
	mov	x20, #0x1000
	movk	x20, #0x0340, lsl #16		/* 0x03401000 */
	mov	x21, #0x00ff			/* Red pixel */
	movk	x21, #0x00ff, lsl #32
	mov	x22, #0xf000
	movk	x22, #0xf, lsl #16		/* 1044480 = 0x0ff000 */
panic_loop:
	str	x21, [x20], #8
	subs	x22, x22, #1
	b.ne	panic_loop

	/* Delay 5 seconds then reset */
	bl	delay_5s
	b	do_reset

/*
 * Main Shim Entry Point
 */
	.text
	.globl _start
_start:
	/* 1. Mask all interrupts */
	msr	daifset, #0xf

	/* 2. Save FDT pointer in x19 and actual Entry PC in x21 */
	mov	x19, x0
	adr	x21, _head

	/* 3. Setup safe stack at 0x00400000 */
	mov	x1, #0x00400000
	mov	sp, x1

	/* 4. Set Exception Vector Base Address */
	adr	x1, vectors
	msr	vbar_el1, x1
	isb

	/* 5. Zero .bss section */
	adr	x1, _bss_start
	adr	x2, _bss_end
1:	cmp	x1, x2
	b.hs	2f
	str	xzr, [x1], #8
	b	1b

2:	/* 6. Call C entry point with FDT pointer in x0, actual Entry PC in x1 */
	mov	x0, x19
	mov	x1, x21
	bl	shim_main

	/* 7. If shim_main ever returns, reset */
	b	do_reset

do_reset:
	/* Write 0 to PS_HOLD (0xfc4ab000) */
	mov	x1, #0xb000
	movk	x1, #0xfc4a, lsl #16		/* 0xfc4ab000 */
	str	wzr, [x1]

	/* In case reset takes a few cycles, loop wfe */
reset_spin:
	wfe
	b	reset_spin

/*
 * Framebuffer Stripe Painter:
 * Display: 1080 x 1920
 *
 * Top Half (Lines 0..959): 32 bpp (Stride = 4352 bytes = 544 64-bit words/line)
 *   Lines   0..319 (320 lines): RED   (0x00FF0000)
 *   Lines 320..639 (320 lines): GREEN (0x0000FF00)
 *   Lines 640..959 (320 lines): BLUE  (0x000000FF)
 *
 * Bottom Half (Lines 960..1919): 24 bpp (Stride = 3240 bytes = 1080 * 3 bytes)
 *   Lines  960..1279 (320 lines): YELLOW  (R=FF, G=FF, B=00)
 *   Lines 1280..1599 (320 lines): CYAN    (R=00, G=FF, B=FF)
 *   Lines 1600..1919 (320 lines): MAGENTA (R=FF, G=00, B=FF)
 */
paint_framebuffer_stripes:
	stp	x29, x30, [sp, #-16]!
	mov	x20, #0x1000
	movk	x20, #0x0340, lsl #16		/* x20 = 0x03401000 */

	/* --- TOP HALF: 32 bpp --- */
	/* Band 1: RED (320 lines, 544 qwords/line = 174080 qwords) */
	/* 32bpp pixel: 0x00FF0000. Double-pixel in 64-bit: 0x00FF000000FF0000 */
	mov	x21, #0x0000
	movk	x21, #0x00ff, lsl #16
	movk	x21, #0x0000, lsl #32
	movk	x21, #0x00ff, lsl #48
	mov	x22, #0xa800
	movk	x22, #0x2, lsl #16		/* 174080 */
paint_red:
	str	x21, [x20], #8
	subs	x22, x22, #1
	b.ne	paint_red

	/* Band 2: GREEN (320 lines) */
	/* Double-pixel: 0x0000FF000000FF00 */
	mov	x21, #0xff00
	movk	x21, #0x0000, lsl #16
	movk	x21, #0xff00, lsl #32
	movk	x21, #0x0000, lsl #48
	mov	x22, #0xa800
	movk	x22, #0x2, lsl #16		/* 174080 */
paint_green:
	str	x21, [x20], #8
	subs	x22, x22, #1
	b.ne	paint_green

	/* Band 3: BLUE (320 lines) */
	/* Double-pixel: 0x000000FF000000FF */
	mov	x21, #0x00ff
	movk	x21, #0x0000, lsl #16
	movk	x21, #0x00ff, lsl #32
	movk	x21, #0x0000, lsl #48
	mov	x22, #0xa800
	movk	x22, #0x2, lsl #16		/* 174080 */
paint_blue:
	str	x21, [x20], #8
	subs	x22, x22, #1
	b.ne	paint_blue

	/* --- BOTTOM HALF: 24 bpp (lines 960..1919) --- */
	/*
	 * Line stride = 3240 bytes.
	 * 320 lines * 3240 bytes = 1,036,800 bytes per band.
	 * Notice 3240 is divisible by 24 (135 chunks of 24 bytes).
	 * In 24 bytes, we have 8 pixels of 3 bytes each:
	 * Yellow (R=FF, G=FF, B=00): repeated pattern of bytes 00 FF FF
	 * In 24 bytes (three 64-bit words):
	 * Word 0: 00 FF FF 00 FF FF 00 FF
	 * Word 1: FF 00 FF FF 00 FF FF 00
	 * Word 2: FF FF 00 FF FF 00 FF FF
	 */
	/* Yellow Band: 320 lines * 3240 bytes = 1,036,800 bytes = 43,200 * 24 bytes */
	mov	x23, #0xff00
	movk	x23, #0x00ff, lsl #16
	movk	x23, #0xffff, lsl #32
	movk	x23, #0xff00, lsl #48		/* 0xFF00FFFF00FFFF00 (Wait, little endian) */
	/* Let's write simple 3-byte stores per pixel to avoid endianness confusion */
	mov	x22, #0x4600
	movk	x22, #0x5, lsl #16		/* 345600 */
paint_yellow:
	mov	w24, #0x00			/* B */
	strb	w24, [x20], #1
	mov	w24, #0xff			/* G */
	strb	w24, [x20], #1
	mov	w24, #0xff			/* R */
	strb	w24, [x20], #1
	subs	x22, x22, #1
	b.ne	paint_yellow

	/* Cyan Band: R=00, G=FF, B=FF */
	mov	x22, #0x4600
	movk	x22, #0x5, lsl #16		/* 345600 */
paint_cyan:
	mov	w24, #0xff			/* B */
	strb	w24, [x20], #1
	mov	w24, #0xff			/* G */
	strb	w24, [x20], #1
	mov	w24, #0x00			/* R */
	strb	w24, [x20], #1
	subs	x22, x22, #1
	b.ne	paint_cyan

	/* Magenta Band: R=FF, G=00, B=FF */
	mov	x22, #0x4600
	movk	x22, #0x5, lsl #16		/* 345600 */
paint_magenta:
	mov	w24, #0xff			/* B */
	strb	w24, [x20], #1
	mov	w24, #0x00			/* G */
	strb	w24, [x20], #1
	mov	w24, #0xff			/* R */
	strb	w24, [x20], #1
	subs	x22, x22, #1
	b.ne	paint_magenta

	ldp	x29, x30, [sp], #16
	ret

/*
 * Delay 5 seconds using virtual counter (cntvct_el0)
 * 19.2 MHz * 5 = 96,000,000 ticks = 0x05b8d800
 */
delay_5s:
	isb
	mrs	x1, cntvct_el0
	mov	x2, #0xd800
	movk	x2, #0x05b8, lsl #16		/* 0x05b8d800 */
	add	x3, x1, x2
	/* Fallback cycle counter: ~100M loops */
	mov	x4, #0x0000
	movk	x4, #0x0600, lsl #16
1:	isb
	mrs	x1, cntvct_el0
	cmp	x1, x3
	b.hs	2f
	subs	x4, x4, #1
	b.ne	1b
2:	ret

/*
 * Ramoops helper routines
 * x28 = buffer base (0xac0be000)
 * Header:
 *   [x28, #0] = sig
 *   [x28, #4] = start (current offset)
 *   [x28, #8] = size  (current length)
 *   [x28, #12] = start of char data
 */
ramoops_putc:
	ldr	w9, [x28, #8]			/* current size */
	mov	w8, #0xf7a0
	movk	w8, #0x3, lsl #16		/* 260,000 bytes cap */
	cmp	w9, w8
	b.hs	9f
	add	x8, x28, #12
	strb	w0, [x8, x9]
	add	w9, w9, #1
	str	w9, [x28, #4]			/* start = size */
	str	w9, [x28, #8]
9:	ret

ramoops_puts:
	stp	x29, x30, [sp, #-16]!
	mov	x7, x0
1:	ldrb	w0, [x7], #1
	cbz	w0, 2f
	bl	ramoops_putc
	b	1b
2:	ldp	x29, x30, [sp], #16
	ret

/* Print x0 as 16-hex-digit (64-bit) */
ramoops_puthex64:
	stp	x29, x30, [sp, #-16]!
	mov	x6, x0
	mov	x5, #60
1:	lsr	x0, x6, x5
	and	x0, x0, #0xf
	cmp	x0, #10
	blo	2f
	add	x0, x0, #('A' - 10)
	b	3f
2:	add	x0, x0, #'0'
3:	bl	ramoops_putc
	subs	x5, x5, #4
	b.ge	1b
	ldp	x29, x30, [sp], #16
	ret

/* Print w0 as 8-hex-digit (32-bit) */
ramoops_puthex32:
	stp	x29, x30, [sp, #-16]!
	mov	w6, w0
	mov	w5, #28
1:	lsr	w0, w6, w5
	and	w0, w0, #0xf
	cmp	w0, #10
	blo	2f
	add	w0, w0, #('A' - 10)
	b	3f
2:	add	w0, w0, #'0'
3:	bl	ramoops_putc
	subs	w5, w5, #4
	b.ge	1b
	ldp	x29, x30, [sp], #16
	ret

/* Print w0 as 2-hex-digit (8-bit) */
ramoops_puthex8:
	stp	x29, x30, [sp, #-16]!
	mov	w6, w0
	mov	w5, #4
1:	lsr	w0, w6, w5
	and	w0, w0, #0xf
	cmp	w0, #10
	blo	2f
	add	w0, w0, #('A' - 10)
	b	3f
2:	add	w0, w0, #'0'
3:	bl	ramoops_putc
	subs	w5, w5, #4
	b.ge	1b
	ldp	x29, x30, [sp], #16
	ret

/*
 * Constant strings
 */
	.section .rodata
str_banner:
	.ascii	"\n=======================================================\n"
	.ascii	"  [ONEPLUS2-SHIM] M1 Bare-Metal Shim Executed Successfully!\n"
	.ascii	"=======================================================\n\0"

str_el:
	.ascii	"  Execution Level : EL\0"

str_fdt_ptr:
	.ascii	"\n  FDT Pointer (x0): 0x\0"

str_fdt_magic:
	.ascii	"\n  FDT Magic       : 0x\0"

str_null:
	.ascii	"(null)\0"

str_paint_start:
	.ascii	"\n  Painting Framebuffer stripes (32bpp top, 24bpp bottom)... \0"

str_paint_done:
	.ascii	"DONE\n  Holding display for 5 seconds...\0"

str_reset:
	.ascii	"\n  Triggering PMIC reset via PS_HOLD (0xfc4ab000)...\n"
	.ascii	"=======================================================\n\0"

str_newline:
	.ascii	"\n\0"

str_exc_header:
	.ascii	"\n!!! UNEXPECTED EXCEPTION !!! Slot: 0x\0"
str_exc_esr:
	.ascii	" ESR_EL1: 0x\0"
str_exc_far:
	.ascii	" FAR_EL1: 0x\0"
str_exc_elr:
	.ascii	" ELR_EL1: 0x\0"
str_exc_newline:
	.ascii	"\n\0"

	.balign 4096
	.globl _shim_end
_shim_end:

