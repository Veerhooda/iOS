ENTRY(_head)

SECTIONS
{
	. = 0x00080000;

	.text : {
		*(.text.head)
		*(.text*)
	}

	.rodata : {
		*(.rodata*)
	}

	.data : {
		*(.data*)
	}

	. = ALIGN(16);
	_bss_start = .;
	.bss : {
		*(.bss*)
		*(COMMON)
	}
	. = ALIGN(16);
	_bss_end = .;

	. = ALIGN(4096);
	_shim_end = .;

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}
}
