ENTRY(_head)

SECTIONS
{
	. = 0x00008000;

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

	. = ALIGN(4096);
	_shim_end = .;

	/DISCARD/ : {
		*(.comment)
		*(.note*)
		*(.eh_frame*)
	}
}
