/* Linker script for the RK3506 M0 audio firmware.
 *
 * The image is linked at address 0, not at its SRAM address. The Cortex-M0
 * has no VTOR and fetches its vectors from 0; the SoC's address converter
 * maps M0 address 0 onto the "code start address" the host passes to the
 * SiP call (0xFFF88000, see picocalc_rk3506_rproc). The core could not run
 * at 0xFFF8xxxx anyway: on ARMv6-M everything from 0xE0000000 up is
 * execute-never. Data accesses to absolute addresses up there are fine, which
 * is how the firmware reaches the ring and the peripherals.
 *
 * 16 KB system SRAM bank, physical 0xFFF88000..0xFFF8C000:
 *   0xFFF88000  = M0 0x0000  image (vectors, code, data, bss) and stack, 4 KB
 *   0xFFF89000              shared header + ring written by the host (shmem.h),
 *                           addressed absolutely, 64 + 8192 B
 * Loading at 0xFFF88000 rather than 0xFFF84000 keeps OP-TEE from switching
 * the SRAM to TCM mode, which would lock the host out of it until reboot.
 */
OUTPUT_ARCH(arm)
ENTRY(Reset_Handler)

MEMORY
{
	RAM (rwx) : ORIGIN = 0x00000000, LENGTH = 0x1000
}

/* Stack grows down from the end of the 4 KB window; the image needs under 1 KB. */
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
