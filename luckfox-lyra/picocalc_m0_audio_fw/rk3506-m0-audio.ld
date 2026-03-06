/* Linker script for RK3506 M0 audio firmware at 0xFFF88000 (avoids OP-TEE SRAM lockdown) */
OUTPUT_ARCH(arm)
ENTRY(Reset_Handler)

MEMORY
{
	RAM (rwx) : ORIGIN = 0xFFF88000, LENGTH = 0x4000
}

_stack_top = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
	.vectors : {
		. = ALIGN(4);
		KEEP(*(.vectors))
	} > RAM

	.text : {
		. = ALIGN(4);
		/* .ramfunc* is placed first so the ISR is nearest the vector table.
		 * No LMA/VMA split or copy loop is needed: this firmware has no flash
		 * region — remoteproc loads the entire image into SRAM at runtime. */
		*(.ramfunc*)
		*(.text*)
		*(.rodata*)
		. = ALIGN(4);
		_etext = .;
	} > RAM

	.resource_table : {
		. = ALIGN(4);
		KEEP(*(.resource_table))
	} > RAM

	.data : {
		. = ALIGN(4);
		_sdata = .;
		*(.data*)
		. = ALIGN(4);
		_edata = .;
	} > RAM

	.bss : {
		. = ALIGN(4);
		_sbss = .;
		*(.bss*)
		*(COMMON)
		. = ALIGN(4);
		_ebss = .;
		. = ALIGN(8);
		_end = .;
	} > RAM
}
