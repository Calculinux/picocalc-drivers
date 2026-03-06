/*
 * SPDX-License-Identifier: GPL-2.0
 * Cortex-M0 vector table and minimal startup (linked at 0xFFF88000)
 */
	.syntax unified
	.cpu cortex-m0
	.thumb

	.section .vectors,"a",%progbits
	.align 2
	.global __vectors
__vectors:
	.word	_stack_top
	.word	Reset_Handler
	.word	NMI_Handler
	.word	HardFault_Handler
	.word	0
	.word	0
	.word	0
	.word	0
	.word	0
	.word	0
	.word	0
	.word	SVC_Handler
	.word	0
	.word	0
	.word	PendSV_Handler
	.word	SysTick_Handler
	.rept 19
	.word	0
	.endr
	.word	TIMER0_CH5_IRQHandler

	.text
	.thumb_func
	.global Reset_Handler
Reset_Handler:
	ldr	r0, =_stack_top
	mov	sp, r0

	/* Zero .bss (uninitialized globals) */
	ldr	r0, =_sbss
	ldr	r1, =_ebss
	movs	r2, #0
.Lbss_loop:
	cmp	r0, r1
	bge	.Lbss_done
	str	r2, [r0]
	adds	r0, #4
	b	.Lbss_loop
.Lbss_done:

	bl	main
	b	.

	.thumb_func
	.weak NMI_Handler
NMI_Handler:		b .

	.thumb_func
	.weak HardFault_Handler
HardFault_Handler:	b .

	.thumb_func
	.weak SVC_Handler
SVC_Handler:		b .

	.thumb_func
	.weak PendSV_Handler
PendSV_Handler:	b .

	.thumb_func
	.weak SysTick_Handler
SysTick_Handler:	b .

	.thumb_func
	.weak Default_Handler
Default_Handler:	b .

	/* TIMER0_CH5_IRQHandler is implemented in isr.S (hand-tuned asm) for minimal stalls. */

	/* _stack_top provided by linker script (end of RAM) */
