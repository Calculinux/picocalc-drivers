/* SPDX-License-Identifier: GPL-2.0 */
/* M0 delta-sigma playback loop: one iteration per TIMER0_CH5 expiry (1 MHz).
 *
 * WHY A LOOP AND NOT AN INTERRUPT HANDLER
 *   Exception entry and exit cost 32 cycles on a Cortex-M0, and a handler has
 *   to load and store every piece of state it touches. The M0 does nothing
 *   else while playing, so m0_play() owns all fourteen general registers,
 *   keeps the modulator state in them, and waits for the tick by reading the
 *   timer's int_pd bit. A plain tick is about 50 cycles instead of about 130.
 *
 * EACH TICK
 *   1. Wait for TIMER_INTSTAT.int_pd, clear it (write 1).
 *   2. Write the GPIO word computed on the PREVIOUS tick (GPIO4_B2 = left,
 *      GPIO4_B3 = right). This is the first thing after the wait, ahead of
 *      anything data-dependent, so every edge has the same latency.
 *   3. Count down to the next sample. Two ticks do extra work:
 *        ticks-left == M0_PREFETCH_AT  check ctrl for STOP, pull the next
 *                                      frame out of the ring into memory,
 *                                      advance/publish read_idx, and work out
 *                                      how many ticks the next sample lasts
 *        ticks-left == 0               swap the prefetched frame in
 *      so the ring is never touched on the tick that changes the input, and
 *      neither heavy tick moves an output edge.
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
 *   scaled by 7/8 at prefetch, and i2 is clamped so that no input (full-scale
 *   noise, Nyquist-rate squares) can run the state into int32 wrap.
 *   check_dsm.c mirrors this arithmetic bit-exactly; run `make check` after
 *   any change here.
 *
 * SAMPLE TIMING
 *   One sample lasts DS_RATE_HZ / M0_SAMPLE_RATE_HZ ticks (125/6 at 1 MHz). The
 *   integer part is M0_TICKS_BASE; the remainder is spread Bresenham-style, so
 *   five samples in six last 21 ticks and the sixth lasts 20.
 *
 * WAITING
 *   Default build: poll int_pd. The poll loop is 6 cycles, so an edge lands
 *   up to 6 cycles (about 3% of a tick at 198 MHz) after the expiry.
 *   M0_TICK_WFI build (make TICK_WFI=1): sleep in WFI between ticks. main.c
 *   sets PRIMASK and enables NVIC line 19, so the pending timer interrupt
 *   wakes the core without being taken. The wake latency is constant and the
 *   core idles between ticks, at the cost of 6 cycles per tick to clear the
 *   NVIC pending bit. If that clear ever loses the race with the timer's
 *   level, WFI just returns at once and the loop polls for that tick.
 *
 * CYCLES (Cortex-M0, zero wait states; ldr/str 2, taken branch 3, ALU 1)
 *    ~50  plain tick
 *    ~64  swap tick (once per sample)
 *   ~120  prefetch tick (once per sample), a few more when read_idx is published
 *   Budget is hclk_m0 / DS_RATE_HZ, e.g. 198 at 198 MHz. Wait states on the
 *   SRAM, APB (timer, GPIO) and literal loads are not known yet: measure on
 *   hardware before shortening DS_PERIOD_TICKS.
 *
 * REGISTERS
 *   r0 i1_l   r1 i2_l   r2 G_l      r8  x_l          r11 GPIO word
 *   r3 i1_r   r4 i2_r   r5 G_r      r9  x_r          r12 GPIO word, both high
 *   r6, r7 scratch                  r10 ticks left   lr  GPIO4 DR address
 */

#include "rk3506_regs.h"
#include "shmem.h"

	.syntax unified
	.cpu cortex-m0
	.thumb

	.equ	TIMER_INTSTAT_ADDR, TIMER0_CH5_BASE + TIMER_INTSTAT
	.equ	GPIO4_DR_ADDR, GPIO4_BASE + GPIO_DR_L
	.equ	RING_IDX_MASK, M0_RING_BYTES - M0_FRAME_BYTES
	.equ	M0_TICKS_BASE, DS_RATE_HZ / M0_SAMPLE_RATE_HZ
	.equ	M0_TICKS_REM, DS_RATE_HZ % M0_SAMPLE_RATE_HZ
	.equ	M0_PREFETCH_AT, 3	@ ticks before the swap
	.equ	M0_READ_IDX_BATCH, 8	@ publish read_idx every 8 samples
	.equ	DSM_CLAMP_MAX, (1 << DSM_CLAMP_SHIFT) - 1

	.if	M0_TICKS_BASE <= M0_PREFETCH_AT + 1
	.error	"tick rate too low for the prefetch schedule"
	.endif
	.if	M0_TICKS_BASE > 254
	.error	"tick rate too high for an 8-bit tick count"
	.endif

/* State only the two heavy ticks use; everything per-tick is in registers. */
	.equ	S_NEXT_L, 0		@ prefetched left sample, already scaled
	.equ	S_NEXT_R, 4
	.equ	S_NEXT_TICKS, 8		@ how many ticks that sample lasts
	.equ	S_READ_IDX, 12		@ ring read index, bytes
	.equ	S_ERR, 16		@ Bresenham remainder
	.equ	S_BATCH, 20		@ samples since read_idx was published

	.section .bss.m0_play_state, "aw", %nobits
	.align	2
m0_play_state:
	.space	24

	.section .ramfunc, "ax", %progbits
	.align	2
	.global	m0_play
	.thumb_func
	.type	m0_play, %function

/* void m0_play(void): timer running, shmem header valid. Returns when
 * shmem->ctrl != M0_CTRL_PLAY, with the pins as the last tick left them. */
m0_play:
	push	{r4-r7, lr}
	mov	r4, r8
	mov	r5, r9
	mov	r6, r10
	mov	r7, r11
	push	{r4-r7}

	ldr	r7, =m0_play_state
	movs	r0, #0
	str	r0, [r7, #S_NEXT_L]
	str	r0, [r7, #S_NEXT_R]
	str	r0, [r7, #S_ERR]
	str	r0, [r7, #S_BATCH]
	movs	r1, #M0_TICKS_BASE
	str	r1, [r7, #S_NEXT_TICKS]
	mov	r10, r1			@ first swap after one base interval
	ldr	r6, =M0_SHMEM_ADDR
	ldr	r1, [r6, #M0_SHMEM_READ_IDX]
	ldr	r2, =RING_IDX_MASK
	ands	r1, r2			@ in range and frame aligned, whatever the host wrote
	str	r1, [r7, #S_READ_IDX]

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
	ldr	r7, =TIMER_INTSTAT_ADDR

.L_wait:
#ifdef M0_TICK_WFI
	wfi				@     woken by the pending (masked) timer IRQ
#endif
	ldr	r6, [r7]		@ 2   int_pd (bit 0; the rest reads 0)
	cmp	r6, #0			@ 1
	beq	.L_wait			@ 1   (3 while waiting)
	str	r6, [r7]		@ 2   write 1 to clear

	mov	r6, r11			@ 1   OUTPUT: bits from the previous tick
	mov	r7, lr			@ 1
	str	r6, [r7]		@ 2
#ifdef M0_TICK_WFI
	ldr	r7, =NVIC_ICPR0		@ 2   timer level is low again: drop the pend
	movs	r6, #1			@ 1
	lsls	r6, r6, #TIMER0_CH5_IRQ	@ 1
	str	r6, [r7]		@ 2
#endif

	mov	r6, r10			@ 1   ticks left in this sample
	subs	r6, #1			@ 1
	mov	r10, r6			@ 1   (high-register mov leaves the flags)
	beq	.L_swap			@ 1
	cmp	r6, #M0_PREFETCH_AT	@ 1
	beq	.L_prefetch		@ 1

.L_dsm:
	mov	r11, r12		@ 1   start from both high; a 0 bit subtracts itself

	add	r0, r8			@ 1   LEFT  i1 += x
	subs	r0, r0, r2		@ 1         i1 -= G
	adds	r1, r1, r0		@ 1         y = i2 + i1
	asrs	r7, r1, #31		@ 1         s = 0 (bit 1) or -1 (bit 0)
	adds	r2, r7, r7		@ 1
	adds	r2, #1			@ 1         2s + 1
	lsls	r2, r2, #DSM_FS_SHIFT	@ 1         G = +FS / -FS
	subs	r1, r1, r2		@ 1         i2 = y - G
	asrs	r6, r1, #DSM_CLAMP_SHIFT @ 1        in range iff this is 0 or -1
	adds	r6, #1			@ 1
	lsrs	r6, r6, #1		@ 1
	bne	.L_clamp_l		@ 1
.L_clamped_l:
	lsls	r7, r7, #GPIO4_B2_BIT	@ 1         0, or -(1 << 10) to clear B2
	add	r11, r7			@ 1

	add	r3, r9			@ 1   RIGHT, same with r3/r4/r5 and B3
	subs	r3, r3, r5		@ 1
	adds	r4, r4, r3		@ 1
	asrs	r7, r4, #31		@ 1
	adds	r5, r7, r7		@ 1
	adds	r5, #1			@ 1
	lsls	r5, r5, #DSM_FS_SHIFT	@ 1
	subs	r4, r4, r5		@ 1
	asrs	r6, r4, #DSM_CLAMP_SHIFT @ 1
	adds	r6, #1			@ 1
	lsrs	r6, r6, #1		@ 1
	bne	.L_clamp_r		@ 1
.L_clamped_r:
	lsls	r7, r7, #GPIO4_B3_BIT	@ 1
	add	r11, r7			@ 1

	ldr	r7, =TIMER_INTSTAT_ADDR	@ 2
	b	.L_wait			@ 3

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

/* SWAP (ticks left == 0): the prefetched frame becomes the modulator input.
 * On an underrun nothing was prefetched, so this re-loads the same values and
 * the output holds. */
.L_swap:
	ldr	r7, =m0_play_state	@ 2
	ldr	r6, [r7, #S_NEXT_L]	@ 2
	mov	r8, r6			@ 1
	ldr	r6, [r7, #S_NEXT_R]	@ 2
	mov	r9, r6			@ 1
	ldr	r6, [r7, #S_NEXT_TICKS]	@ 2
	mov	r10, r6			@ 1
	b	.L_dsm			@ 3

/* PREFETCH (ticks left == M0_PREFETCH_AT). Needs more than two scratch
 * registers, so it borrows r0-r2 from the left channel for the duration. */
.L_prefetch:
	push	{r0-r2}			@ 4
	ldr	r7, =M0_SHMEM_ADDR	@ 2
	ldr	r6, [r7, #M0_SHMEM_CTRL] @ 2
	cmp	r6, #M0_CTRL_PLAY	@ 1
	bne	.L_stop			@ 1
	ldr	r6, =m0_play_state	@ 2

	/* Length of the next sample: base ticks, plus one when the remainder wraps. */
	movs	r2, #M0_TICKS_BASE	@ 1
	ldr	r0, [r6, #S_ERR]	@ 2
	ldr	r1, =M0_TICKS_REM	@ 2
	adds	r0, r0, r1		@ 1
	ldr	r1, =M0_SAMPLE_RATE_HZ	@ 2
	cmp	r0, r1			@ 1
	blo	1f			@ 1/3
	subs	r0, r0, r1		@ 1
	adds	r2, #1			@ 1
1:	str	r0, [r6, #S_ERR]	@ 2
	str	r2, [r6, #S_NEXT_TICKS]	@ 2

	/* Next frame, unless the ring is empty (then S_NEXT_* stay: hold). */
	ldr	r0, [r6, #S_READ_IDX]	@ 2
	ldr	r1, [r7, #M0_SHMEM_WRITE_IDX] @ 2
	cmp	r0, r1			@ 1
	beq	.L_prefetch_done	@ 1
	adds	r7, #M0_HEADER_SIZE	@ 1   ring base
	ldr	r1, [r7, r0]		@ 2   one S16_LE frame: L in [15:0], R in [31:16]
	sxth	r2, r1			@ 1   left
	asrs	r1, r1, #16		@ 1   right
	asrs	r7, r2, #3		@ 1
	subs	r2, r2, r7		@ 1   x -= x >> 3 (7/8 headroom)
	asrs	r7, r1, #3		@ 1
	subs	r1, r1, r7		@ 1
	str	r2, [r6, #S_NEXT_L]	@ 2
	str	r1, [r6, #S_NEXT_R]	@ 2
	adds	r0, #M0_FRAME_BYTES	@ 1
	ldr	r1, =RING_IDX_MASK	@ 2
	ands	r0, r1			@ 1
	str	r0, [r6, #S_READ_IDX]	@ 2
	/* Tell the host how far we have read, every M0_READ_IDX_BATCH samples. */
	ldr	r1, [r6, #S_BATCH]	@ 2
	adds	r1, #1			@ 1
	cmp	r1, #M0_READ_IDX_BATCH	@ 1
	blo	2f			@ 1/3
	ldr	r7, =M0_SHMEM_ADDR	@ 2
	str	r0, [r7, #M0_SHMEM_READ_IDX] @ 2
	movs	r1, #0			@ 1
2:	str	r1, [r6, #S_BATCH]	@ 2
.L_prefetch_done:
	pop	{r0-r2}			@ 4
	b	.L_dsm			@ 3

.L_stop:
	pop	{r0-r2}
	pop	{r4-r7}
	mov	r8, r4
	mov	r9, r5
	mov	r10, r6
	mov	r11, r7
	pop	{r4-r7, pc}

	.pool
	.size	m0_play, . - m0_play
