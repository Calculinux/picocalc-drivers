/* SPDX-License-Identifier: GPL-2.0 */
/* Loops whose instruction sequence has to be known exactly: the calibration
 * loops (cycle counts) and the tick waits (same shape as the audio firmware's
 * wait in play.S, so the latency measured here is the latency it will see). */

	.syntax unified
	.cpu cortex-m0
	.thumb

	.equ	TIMER_CURR0, 0x08
	.equ	TIMER_INTSTAT, 0x18

	.text

/* void diag_loop_empty(uint32_t n): 4 cycles per iteration (subs 1, taken bne 3) */
	.global	diag_loop_empty
	.thumb_func
	.type	diag_loop_empty, %function
diag_loop_empty:
1:	subs	r0, #1
	bne	1b
	bx	lr
	.size	diag_loop_empty, . - diag_loop_empty

/* void diag_loop_str(uint32_t n, volatile uint32_t *addr, uint32_t val):
 * the empty loop plus one store (2 cycles at zero wait states) */
	.global	diag_loop_str
	.thumb_func
	.type	diag_loop_str, %function
diag_loop_str:
1:	str	r2, [r1]
	subs	r0, #1
	bne	1b
	bx	lr
	.size	diag_loop_str, . - diag_loop_str

/* void diag_loop_ldr(uint32_t n, volatile uint32_t *addr):
 * the empty loop plus one load (2 cycles at zero wait states) */
	.global	diag_loop_ldr
	.thumb_func
	.type	diag_loop_ldr, %function
diag_loop_ldr:
1:	ldr	r2, [r1]
	subs	r0, #1
	bne	1b
	bx	lr
	.size	diag_loop_ldr, . - diag_loop_ldr

/* uint32_t diag_wait_poll(uint32_t timer_base): poll int_pd, then return the
 * timer's count (it restarted from 0 at the expiry) and clear the status. */
	.global	diag_wait_poll
	.thumb_func
	.type	diag_wait_poll, %function
diag_wait_poll:
1:	ldr	r3, [r0, #TIMER_INTSTAT]
	cmp	r3, #0
	beq	1b
	ldr	r2, [r0, #TIMER_CURR0]
	str	r3, [r0, #TIMER_INTSTAT]
	mov	r0, r2
	bx	lr
	.size	diag_wait_poll, . - diag_wait_poll

/* uint32_t diag_wait_wfi(uint32_t timer_base): one WFI, then the same check.
 * Returns 0xFFFFFFFF if WFI came back with no expiry pending. */
	.global	diag_wait_wfi
	.thumb_func
	.type	diag_wait_wfi, %function
diag_wait_wfi:
	wfi
	ldr	r3, [r0, #TIMER_INTSTAT]
	cmp	r3, #0
	beq	2f
	ldr	r2, [r0, #TIMER_CURR0]
	str	r3, [r0, #TIMER_INTSTAT]
	mov	r0, r2
	bx	lr
2:	movs	r0, #0
	mvns	r0, r0
	bx	lr
	.size	diag_wait_wfi, . - diag_wait_wfi
