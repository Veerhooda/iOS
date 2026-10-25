/*
 * OnePlus 2 Userspace Init (PID 1)
 *
 * M4 Bounded Init:
 * 1. Aligns sp to 16 bytes.
 * 2. Opens /dev/console (O_RDWR = 2). If open fails, exits with code 2.
 * 3. Writes greeting string to /dev/console.
 * 4. Calls exit(0).
 * 5. Kernel exit1() panics with "init died (signal 0, exit 0)",
 *    triggering ramoops save and autonomous PS_HOLD warm reboot to TWRP.
 */

#define SYS_exit	1
#define SYS_write	4
#define SYS_open	5
#define O_RDWR		2

	.text
	.globl	_start
	.p2align 2
_start:
	/*
	 * Align sp to 16 bytes. Kernel enters with sp at argc,
	 * which may not be 16-byte aligned.
	 */
	mov	x9, sp
	and	x9, x9, #~15
	mov	sp, x9

	/* Open /dev/console */
	adr	x0, dev_console
	mov	x1, #O_RDWR
	mov	x2, #0
	mov	x16, #SYS_open
	svc	#0x80
	b.cs	open_failed

	/* Save fd */
	mov	x19, x0

	/* Write greeting */
	mov	x0, x19
	adr	x1, hello_msg
	adr	x2, hello_msg_end
	sub	x2, x2, x1
	mov	x16, #SYS_write
	svc	#0x80

	/* Exit cleanly (exit 0) -> kernel panics "init died" -> PS_HOLD reset */
	mov	x0, #0
	mov	x16, #SYS_exit
	svc	#0x80

	/* Should not be reached */
hang:
	b	hang

open_failed:
	/* If open fails, exit with 2 -> still panics kernel and resets */
	mov	x0, #2
	mov	x16, #SYS_exit
	svc	#0x80
	b	hang

	.p2align 3
dev_console:
	.asciz	"/dev/console"
hello_msg:
	.ascii	"\n*** OnePlus 2 userspace: hello from /sbin/launchd (PID 1) ***\n"
hello_msg_end:
