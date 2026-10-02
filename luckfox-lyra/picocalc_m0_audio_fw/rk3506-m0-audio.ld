/* Linker script for the RK3506 M0 audio firmware.
 *
 * The image is linked at address 0, not at its SRAM address. The Cortex-M0
 * has no VTOR and fetches its vectors from 0; the SoC's address converter
 * maps M0 address 0 onto the "code start address" the host passes to the
 * SiP call (see picocalc_rk3506_rproc). The core could not run at
 * 0xFFF8xxxx anyway: on ARMv6-M everything from 0xE0000000 up is
 * execute-never. Data accesses to absolute addresses up there are fine, which
 * is how the firmware reaches the ring and the peripherals.
 *
 * The same image runs in either mode:
 *   bus mode   loaded at 0xFFF88000; code is fetched over the SoC bus
 *              (about 2.5 cycles per instruction) and can be reloaded
 *   TCM mode   loaded at 0xFFF84000; OP-TEE turns that SRAM into the M0's
 *              tightly-coupled memory (1 cycle per instruction) and Linux
 *              cannot reach it again until reboot
 * Image, data, bss and stack all fit in the first 8 KB of the window; the
 * shared header and ring are elsewhere (shmem.h).
 */
OUTPUT_ARCH(arm)
ENTRY(Reset_Handler)

MEMORY
{
	RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 0x2000
}

/* Stack grows down from the end of the 8 KB window; the image needs about half. */
_stack_top = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
	.vectors : {
		. = ALIGN(4);
		KEEP(*(.vectors))
	} > RAM

	.text : {
		. = ALIGN(4);
		/* .ramfunc* is placed first so the playback loop is nearest the vector table.
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

ASSERT(_end + 0x200 <= _stack_top, "image leaves less than 512 bytes of stack")
