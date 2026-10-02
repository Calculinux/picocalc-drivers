/* SPDX-License-Identifier: GPL-2.0 */
/* M0 delta-sigma playback loop: one iteration per SysTick tick.
 *
 * WHY A LOOP AND NOT AN INTERRUPT HANDLER
 *   Exception entry and exit cost 32 cycles on a Cortex-M0, and a handler has
 *   to load and store every piece of state it touches. The M0 does nothing
 *   else while playing, so m0_play() owns all fourteen general registers and
 *   keeps the modulator state in them.
 *
 * THE TICK
 *   SysTick, on the core clock, with its exception enabled but masked by
 *   PRIMASK: the pending exception wakes WFI and is never taken. Measured on
 *   an RK3506 (picocalc_m0_diag_fw): the wake is a constant 3 cycles after
 *   the reload and a GPIO write a constant 33 cycles, so every output edge
 *   lands the same number of core cycles after its tick. Polling a TIMER0
 *   status bit instead costs about 30 cycles per access and jitters by 50.
 *
 * EACH TICK
 *   1. WFI until SysTick is pending.
 *   2. Write the GPIO word computed on the PREVIOUS tick (GPIO4_B2 = left,
 *      GPIO4_B3 = right), ahead of anything data-dependent.
 *   3. Clear the pending bit; count down to the next sample. The last four
 *      ticks of a sample each do one piece of the per-sample work, so that no
 *      single tick carries all of it:
 *        3 left  check ctrl for STOP; work out how long the next sample lasts
 *        2 left  read the next frame from the ring, scale it
 *        1 left  advance read_idx, publish it every M0_READ_IDX_BATCH samples
 *        0 left  swap the prefetched frame in
 *   4. Run the second-order modulator for both channels, build the next GPIO word.
 *
 * MODULATOR (per channel, FS = 32768, G = +FS when the output bit is 1, else -FS)
 *      i1 += x - G_prev
 *      y   = i2 + i1
 *      s   = y >> 31                 @ 0: bit = 1, -1: bit = 0
 *      G   = (2s + 1) << 15
 *      i2  = clamp(y - G)
 *   which is the standard MOD2: out = x + (1 - z^-1)^2 * e (unity signal gain,
 *   second-order noise shaping). Both stages need the same +/-FS feedback; a
 *   half-scale stage 1 doubles the signal gain and overloads at |x| > FS/2.
 *   A single-bit MOD2 is only well behaved short of full scale, so samples are
 *   scaled by 7/8 at fetch, and i2 is clamped so that no input (full-scale
 *   noise, Nyquist-rate squares) can run the state into int32 wrap.
 *   check_dsm.c mirrors this arithmetic bit-exactly; run `make check` after
 *   any change here.
 *
 * SAMPLE TIMING
 *   A sample lasts ticks_base ticks, plus one whenever the accumulated
 *   ticks_rem reaches ticks_den (shmem.h). The host works these out from the
 *   core clock; main.c copies them into m0_play_state.
 *
 * COST
 *   Do not count cycles from the instruction listing: this image runs from
 *   SRAM over the bus, at about 2.5 cycles per straight-line instruction and
 *   10 for a two-instruction loop, and a GPIO write is 33. Build with
 *   PROFILE=1: the loop then records the lowest SysTick count it sees at the
 *   end of a tick's work and how often a tick ran into the next, and the
 *   host driver reports both when playback stops.
 *   Measured that way at 187.5 MHz: the heaviest tick (3 left) takes about
 *   295 cycles, the other three per-sample ticks 265-285, a plain tick under
 *   250. So 600 kHz (312 cycles) fits everything and each step above it
 *   pushes one more of the four over. A tick that overruns only delays its
 *   own edge and the next tick starts late; none are lost unless the work
 *   exceeds two ticks. A few dozen ticks a second are stretched further by
 *   other traffic on the bus the code is fetched over.
 *
 * REGISTERS
 *   r0 i1_l   r1 i2_l   r2 G_l      r8  x_l          r11 GPIO word
 *   r3 i1_r   r4 i2_r   r5 G_r      r9  x_r          r12 GPIO word, both high
 *   r6, r7 scratch                  r10 ticks left   lr  GPIO4 DR address
 */

#include "rk3506_regs.h"
#include "shmem.h"
#include "play.h"

	.syntax unified
	.cpu cortex-m0
	.thumb

	.equ	GPIO4_DR_ADDR, GPIO4_BASE + GPIO_DR_L
	.equ	RING_IDX_MASK, M0_RING_BYTES - M0_FRAME_BYTES
	.equ	M0_READ_IDX_BATCH, 8	@ publish read_idx every 8 samples
	.equ	DSM_CLAMP_MAX, (1 << DSM_CLAMP_SHIFT) - 1
	.equ	PENDST_TO_SIGN, 31 - ICSR_PENDSTSET_BIT

	.section .bss.m0_play_state, "aw", %nobits
	.align	2
	.global	m0_play_state
m0_play_state:
	.space	PS_SIZE

	.section .ramfunc, "ax", %progbits
	.align	2
	.global	m0_play
	.thumb_func
	.type	m0_play, %function

m0_play:
	push	{r4-r7, lr}
	mov	r4, r8
	mov	r5, r9
	mov	r6, r10
	mov	r7, r11
	push	{r4-r7}

	ldr	r7, =m0_play_state
	movs	r0, #0
	str	r0, [r7, #PS_NEXT_L]
	str	r0, [r7, #PS_NEXT_R]
	str	r0, [r7, #PS_ERR]
	str	r0, [r7, #PS_BATCH]
	str	r0, [r7, #PS_FETCHED]
	str	r0, [r7, #PS_OVERRUNS]
	mvns	r1, r0
	str	r1, [r7, #PS_MIN_CVR]
	ldr	r1, [r7, #PS_TICKS_BASE]
	str	r1, [r7, #PS_NEXT_TICKS]
	mov	r10, r1			@ first swap after one base interval
	ldr	r6, =M0_SHMEM_ADDR
	ldr	r1, [r6, #M0_SHMEM_READ_IDX]
	ldr	r2, =RING_IDX_MASK
	ands	r1, r2			@ in range and frame aligned, whatever the host wrote
	str	r1, [r7, #PS_READ_IDX]

	mov	r8, r0			@ x_l = x_r = 0
	mov	r9, r0
	movs	r1, #0			@ integrators
	movs	r3, #0
	movs	r4, #0
	movs	r2, #1
	lsls	r2, r2, #DSM_FS_SHIFT	@ G_l = +FS
	negs	r5, r2			@ G_r = -FS, so the idle patterns interleave
	ldr	r6, =GPIO4_DR_ADDR
	mov	lr, r6
	ldr	r6, =GPIO4_DR_B23_HIGH
	mov	r12, r6
	ldr	r6, =GPIO4_DR_B23_LOW
	mov	r11, r6
	ldr	r7, =SCB_ICSR

.L_wait:				@ r7 = &SCB_ICSR
#ifdef M0_PROFILE
	/* End of this tick's work. Already pending: the tick overran. Otherwise
	 * remember the lowest SysTick count, i.e. the longest tick so far. */
	ldr	r6, [r7]
	lsls	r6, r6, #PENDST_TO_SIGN
	bpl	1f
	ldr	r7, =m0_play_state
	ldr	r6, [r7, #PS_OVERRUNS]
	adds	r6, #1
	str	r6, [r7, #PS_OVERRUNS]
	b	2f
1:	ldr	r7, =SYST_CVR
	ldr	r6, [r7]
	ldr	r7, =m0_play_state
	ldr	r7, [r7, #PS_MIN_CVR]
	cmp	r6, r7
	bhs	2f
	ldr	r7, =m0_play_state
	str	r6, [r7, #PS_MIN_CVR]
2:	ldr	r7, =SCB_ICSR
#endif
.L_sleep:
	wfi				@ woken by the pending (masked) SysTick
	ldr	r6, [r7]
	lsls	r6, r6, #PENDST_TO_SIGN	@ PENDSTSET into the sign bit
	bpl	.L_sleep		@ woken by something else: not a tick

	mov	r6, r11			@ OUTPUT: bits from the previous tick
	mov	r7, lr
	str	r6, [r7]

	ldr	r7, =SCB_ICSR
	movs	r6, #1
	lsls	r6, r6, #ICSR_PENDSTCLR_BIT
	str	r6, [r7]		@ tick taken

	mov	r6, r10			@ ticks left in this sample
	subs	r6, #1
	mov	r10, r6			@ (high-register mov leaves the flags)
	beq	.L_swap
	cmp	r6, #3
	bls	.L_slow

.L_dsm:
	mov	r11, r12		@ start from both high; a 0 bit subtracts itself

	add	r0, r8			@ LEFT  i1 += x
	subs	r0, r0, r2		@       i1 -= G
	adds	r1, r1, r0		@       y = i2 + i1
	asrs	r7, r1, #31		@       s = 0 (bit 1) or -1 (bit 0)
	adds	r2, r7, r7
	adds	r2, #1			@       2s + 1
	lsls	r2, r2, #DSM_FS_SHIFT	@       G = +FS / -FS
	subs	r1, r1, r2		@       i2 = y - G
	asrs	r6, r1, #DSM_CLAMP_SHIFT @      in range iff this is 0 or -1
	adds	r6, #1
	lsrs	r6, r6, #1
	bne	.L_clamp_l
.L_clamped_l:
	lsls	r7, r7, #GPIO4_B2_BIT	@       0, or -(1 << 10) to clear B2
	add	r11, r7

	add	r3, r9			@ RIGHT, same with r3/r4/r5 and B3
	subs	r3, r3, r5
	adds	r4, r4, r3
	asrs	r7, r4, #31
	adds	r5, r7, r7
	adds	r5, #1
	lsls	r5, r5, #DSM_FS_SHIFT
	subs	r4, r4, r5
	asrs	r6, r4, #DSM_CLAMP_SHIFT
	adds	r6, #1
	lsrs	r6, r6, #1
	bne	.L_clamp_r
.L_clamped_r:
	lsls	r7, r7, #GPIO4_B3_BIT
	add	r11, r7

	ldr	r7, =SCB_ICSR
	b	.L_wait

/* i2 left the clamp window: saturate to +MAX or -MAX-1 by its sign. */
.L_clamp_l:
	asrs	r1, r1, #31
	ldr	r6, =DSM_CLAMP_MAX
	eors	r1, r6
	b	.L_clamped_l
.L_clamp_r:
	asrs	r4, r4, #31
	ldr	r6, =DSM_CLAMP_MAX
	eors	r4, r6
	b	.L_clamped_r

/* 0 left: the prefetched frame becomes the modulator input. On an underrun
 * nothing was fetched, so this re-loads the same values and the output holds. */
.L_swap:
	ldr	r7, =m0_play_state
	ldr	r6, [r7, #PS_NEXT_L]
	mov	r8, r6
	ldr	r6, [r7, #PS_NEXT_R]
	mov	r9, r6
	ldr	r6, [r7, #PS_NEXT_TICKS]
	mov	r10, r6
	b	.L_dsm

.L_slow:				@ r6 = ticks left: 3, 2 or 1
	cmp	r6, #2
	beq	.L_fetch
	bhi	.L_sched

/* 1 left: step read_idx past the frame fetched on the previous tick. */
	ldr	r7, =m0_play_state
	ldr	r6, [r7, #PS_FETCHED]
	cmp	r6, #0
	beq	.L_dsm
	push	{r0}
	ldr	r0, [r7, #PS_READ_IDX]
	adds	r0, #M0_FRAME_BYTES
	ldr	r6, =RING_IDX_MASK
	ands	r0, r6
	str	r0, [r7, #PS_READ_IDX]
	ldr	r6, [r7, #PS_BATCH]
	adds	r6, #1
	cmp	r6, #M0_READ_IDX_BATCH
	blo	2f
	ldr	r6, =M0_SHMEM_ADDR	@ tell the host how far we have read
	str	r0, [r6, #M0_SHMEM_READ_IDX]
#ifdef M0_PROFILE
	ldr	r0, [r7, #PS_MIN_CVR]
	str	r0, [r6, #M0_SHMEM_STAT_MIN_CVR]
	ldr	r0, [r7, #PS_OVERRUNS]
	str	r0, [r6, #M0_SHMEM_STAT_OVERRUNS]
#endif
	movs	r6, #0
2:	str	r6, [r7, #PS_BATCH]
	pop	{r0}
	b	.L_dsm

/* 2 left: next frame out of the ring, unless it is empty (then PS_NEXT_* stay
 * as they are and the output holds). */
.L_fetch:
	push	{r0, r1}
	ldr	r7, =m0_play_state
	ldr	r6, =M0_SHMEM_ADDR
	ldr	r0, [r7, #PS_READ_IDX]
	ldr	r1, [r6, #M0_SHMEM_WRITE_IDX]
	subs	r1, r1, r0		@ 0: empty, and that is what PS_FETCHED gets
	beq	1f
	adds	r6, #M0_HEADER_SIZE	@ ring base
	ldr	r1, [r6, r0]		@ one S16_LE frame: L in [15:0], R in [31:16]
	sxth	r6, r1			@ left
	asrs	r1, r1, #16		@ right
	asrs	r0, r6, #3
	subs	r6, r6, r0		@ x -= x >> 3 (7/8 headroom)
	asrs	r0, r1, #3
	subs	r1, r1, r0
	str	r6, [r7, #PS_NEXT_L]
	str	r1, [r7, #PS_NEXT_R]
	movs	r1, #1
1:	str	r1, [r7, #PS_FETCHED]
	pop	{r0, r1}
	b	.L_dsm

/* 3 left: STOP check, and the length of the next sample: base ticks, plus one
 * when the accumulated remainder reaches the divisor. */
.L_sched:
	ldr	r7, =M0_SHMEM_ADDR
	ldr	r6, [r7, #M0_SHMEM_CTRL]
	cmp	r6, #M0_CTRL_PLAY
	bne	.L_stop
	push	{r0, r1}
	ldr	r7, =m0_play_state
	ldr	r0, [r7, #PS_ERR]
	ldr	r1, [r7, #PS_TICKS_REM]
	adds	r0, r0, r1
	ldr	r1, [r7, #PS_TICKS_DEN]
	ldr	r6, [r7, #PS_TICKS_BASE]
	cmp	r0, r1
	blo	1f
	subs	r0, r0, r1
	adds	r6, #1
1:	str	r0, [r7, #PS_ERR]
	str	r6, [r7, #PS_NEXT_TICKS]
	pop	{r0, r1}
	b	.L_dsm

.L_stop:
	pop	{r4-r7}
	mov	r8, r4
	mov	r9, r5
	mov	r10, r6
	mov	r11, r7
	pop	{r4-r7, pc}

	.pool
	.size	m0_play, . - m0_play
