/* SPDX-License-Identifier: GPL-2.0 */
/* M0 delta-sigma playback loop: one output bit per channel per SysTick tick.
 *
 * WHY A LOOP AND NOT AN INTERRUPT HANDLER
 *   Exception entry and exit cost 32 cycles on a Cortex-M0, and a handler has
 *   to load and store every piece of state it touches. The M0 does nothing
 *   else while playing, so m0_play() owns every register, SP included, and
 *   keeps the modulator state in them.
 *
 * THE TICK
 *   SysTick, on the core clock, with its exception enabled but masked by
 *   PRIMASK: the pending exception wakes WFI and is never taken. Measured on
 *   an RK3506 (picocalc_m0_diag_fw): the wake is a constant 3 cycles after
 *   the reload and a GPIO write lands a constant time after its instruction,
 *   with no spurious wakes in thousands of ticks, so the loop does not
 *   re-check why it woke. The GPIO write is the first instruction after WFI:
 *   every output edge lands the same number of core cycles after its tick,
 *   whatever else the tick does. main.c makes the M0's bus writes
 *   "bufferable", so that write costs the loop about 5 cycles, not 32; the
 *   modulators run while it completes, and the first bus access of any step
 *   comes late enough in the tick not to queue behind it.
 *
 * ONE TICK (the TICK macro)
 *      wfi
 *      GPIO <- word computed on the previous tick (B2 = left, B3 = right)
 *      clear SysTick's pending bit
 *      both modulators, next GPIO word
 *
 * THE MODULATOR (per channel; FS = 32768; G = +FS for an output 1, -FS for 0)
 *      i1 += x                      @ i1 already has the previous -G in it
 *      y   = i2 + i1
 *      s   = y >> 31                @ 0: output 1, -1: output 0
 *      G   = FS + (s << 16)
 *      i2  = y - G
 *      i1 -= G
 *   which is the standard MOD2: out = x + (1 - z^-1)^2 * e (unity signal gain,
 *   second-order noise shaping). Both stages need the same +/-FS feedback.
 *   A single-bit MOD2 is only well behaved short of full scale, so samples are
 *   scaled by 7/8 as they are fetched, and i2 is clamped so that no input
 *   (full-scale noise, Nyquist-rate squares) can run the state into int32
 *   wrap. The clamp runs once per sample, not per tick: in simulation the
 *   worst inputs then reach 2^24 at most. check_dsm.c mirrors this arithmetic
 *   bit-exactly; run `make check` after any change here.
 *
 * ONE SAMPLE
 *   The per-sample work is cut into M0_STEP_TICKS steps of at most a dozen
 *   cycles, and each of the first M0_STEP_TICKS ticks of a sample carries one,
 *   as straight-line code: no tick is much longer than a plain one and there
 *   is nothing to dispatch. Then come the plain ticks, in a counted loop.
 *      1  stop if ctrl says so
 *      2  how many plain ticks this sample has
 *      3  snapshot the host's write_idx
 *      4  move on to the next frame, unless that is where the host is writing
 *      5  fetch the current frame
 *      6  left sample in; clamp left
 *      7  right sample in; clamp right
 *      8  work out read_idx
 *      9  publish read_idx
 *     (10-12 in PROFILE builds: publish the profile figures, count the sample)
 *   Left and right change one tick apart. On an underrun step 4 stays on the
 *   same frame, step 5 fetches it again and the output holds; at the start of
 *   a stream that frame is the one before read_idx, which the host zeroes.
 *
 * SAMPLE LENGTH
 *   A sample lasts PS_PLAIN + M0_STEP_TICKS ticks, plus one whenever the
 *   32-bit fraction PS_FRAC, added to PS_ACC each sample, carries out.
 *   main.c sets both from the host's figures (shmem.h).
 *
 * COST
 *   Do not count cycles from the listing alone: in bus mode this image runs
 *   at about 2.5 cycles per instruction (1 as TCM). Build with PROFILE=1: every tick then records the lowest
 *   SysTick count seen at the end of its work and whether it ran into the
 *   next tick, and the host driver reports both when a stream ends. That
 *   costs about 17 cycles per tick, 9 of which are included in the figure.
 *   It also logs every overrun and every empty-ring event with the sample
 *   number it happened in (shmem.h, M0_TRACE_ADDR); m0trace reads the log.
 *   Measured that way at a 187.5 MHz core clock: as TCM every tick fits in
 *   59 cycles (3.2 MHz) and not in 57; in bus mode in 156 (1.2 MHz) and not
 *   in 134. A tick that overruns only delays its own edge and the next tick
 *   starts late; none are lost unless the work exceeds two ticks.
 *
 * REGISTERS
 *   r0 i1_l   r1 i2_l           r8  x_l           r11 scratch / parked GPIO word
 *   r3 i1_r   r4 i2_r           r9  x_r           r12 GPIO word, both high
 *   r2 FS     r5 plain ticks    r10 &SCB_ICSR     lr  GPIO4 DR address
 *   r6, r7 scratch              sp  m0_play_state
 *   Entering a tick, r7 holds the GPIO word to write and r6 the GPIO address.
 */

#include "rk3506_regs.h"
#include "shmem.h"
#include "play.h"

	.syntax unified
	.cpu cortex-m0
	.thumb

	.equ	GPIO4_DR_ADDR, GPIO4_BASE + GPIO_DR_L
	.equ	RING_IDX_MASK, M0_RING_BYTES - M0_FRAME_BYTES
	.equ	DSM_CLAMP_MAX, (1 << DSM_CLAMP_SHIFT) - 1
	.equ	PENDST_TO_SIGN, 31 - ICSR_PENDSTSET_BIT

/* For check_play.py, which has to know how this build schedules a sample */
	.global	m0_step_ticks
	.equ	m0_step_ticks, M0_STEP_TICKS

	.section .bss.m0_play_state, "aw", %nobits
	.align	3
	.global	m0_play_state
m0_play_state:
	.space	PS_SIZE

/* Wait for the tick, put out the bits, run both modulators. Leaves the next
 * GPIO word in r7; r6 is free. */
	.macro	TICK
	wfi
	str	r7, [r6]		@ OUTPUT: bits from the previous tick
	mov	r7, r10
	movs	r6, #1
	lsls	r6, r6, #ICSR_PENDSTCLR_BIT
	str	r6, [r7]		@ tick taken: clear SysTick's pending bit

	add	r0, r8			@ LEFT  i1 += x
	adds	r1, r1, r0		@       y = i2 + i1
	asrs	r7, r1, #31		@       s = 0 (bit 1) or -1 (bit 0)
	lsls	r6, r7, #16
	adds	r6, r6, r2		@       G = FS + (s << 16)
	subs	r1, r1, r6		@       i2 = y - G
	subs	r0, r0, r6		@       i1 -= G
	lsls	r7, r7, #GPIO4_B2_BIT	@       0, or -(1 << 10) to clear B2
	mov	r11, r7

	add	r3, r9			@ RIGHT, same with r3/r4 and B3
	adds	r4, r4, r3
	asrs	r7, r4, #31
	lsls	r6, r7, #16
	adds	r6, r6, r2
	subs	r4, r4, r6
	subs	r3, r3, r6
	lsls	r7, r7, #GPIO4_B3_BIT
	add	r7, r11
	add	r7, r12			@ both-high word minus the bits that are 0
	.endm

/* PROFILE builds: append (sample number << 5) | code to the event log.
 * Uses r6, r7. Two bus writes, so only for things that are already slow. */
	.macro	TRACE_EVENT code
	ldr	r7, [sp, #PS_EVENTS]
	adds	r7, #1
	str	r7, [sp, #PS_EVENTS]
	ldr	r6, [sp, #PS_K_TRACE]
	str	r7, [r6]		@ word 0: events so far
	lsls	r7, r7, #24
	lsrs	r7, r7, #22		@ (n & 255) * 4
	adds	r6, r6, r7
	ldr	r7, [sp, #PS_SAMPLE_NO]
	lsls	r7, r7, #5
	adds	r7, #\code
	str	r7, [r6, #4]
	.endm

/* End of a tick's work, PROFILE builds: already pending means the tick
 * overran; otherwise remember the lowest SysTick count, i.e. the longest
 * tick so far. Uses r6, r7. */
	.macro	PROFILE_TICK code
#ifdef M0_PROFILE
	mov	r7, r10
	ldr	r6, [r7]
	lsls	r6, r6, #PENDST_TO_SIGN
	bpl	1f
	ldr	r6, [sp, #PS_OVERRUNS]
	adds	r6, #1
	str	r6, [sp, #PS_OVERRUNS]
	TRACE_EVENT \code
	b	2f
1:	ldr	r7, [sp, #PS_K_CVR]
	ldr	r6, [r7]
	ldr	r7, [sp, #PS_MIN_CVR]
	cmp	r6, r7
	bhs	2f
	str	r6, [sp, #PS_MIN_CVR]
2:
#endif
	.endm

/* A step tick: TICK, then a step that may use r6 and r7. */
	.macro	STEP
	TICK
	mov	r11, r7			@ park the GPIO word
	.endm

	.macro	STEP_END code
	PROFILE_TICK \code
	mov	r7, r11
	mov	r6, lr
	.endm

/* Saturate an i2 that has left the clamp window to +MAX or -MAX-1. Uses r6. */
	.macro	CLAMP reg
	asrs	r6, \reg, #DSM_CLAMP_SHIFT	@ in range iff this is 0 or -1
	adds	r6, #1
	lsrs	r6, r6, #1
	beq	1f
	asrs	\reg, \reg, #31
	ldr	r6, [sp, #PS_K_CLAMP]
	eors	\reg, r6
1:
	.endm

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

	/* SP becomes the state pointer: nothing below uses a stack, and
	 * SP-relative loads need no address register. */
	mov	r7, sp
	ldr	r6, =m0_play_state
	mov	sp, r6
	str	r7, [sp, #PS_SAVED_SP]

	ldr	r1, =M0_SHMEM_ADDR
	str	r1, [sp, #PS_K_SHMEM]
	ldr	r0, =M0_SHMEM_ADDR + M0_HEADER_SIZE
	str	r0, [sp, #PS_K_RING]
	ldr	r2, =RING_IDX_MASK
	str	r2, [sp, #PS_K_MASK]
	ldr	r0, =DSM_CLAMP_MAX
	str	r0, [sp, #PS_K_CLAMP]
	ldr	r0, =SYST_CVR
	str	r0, [sp, #PS_K_CVR]
#ifdef M0_PROFILE
	ldr	r0, =M0_TRACE_ADDR
	str	r0, [sp, #PS_K_TRACE]
	movs	r3, #0
	str	r3, [r0]		@ empty log
	str	r3, [sp, #PS_EVENTS]
	str	r3, [sp, #PS_SAMPLE_NO]
#endif

	ldr	r0, [r1, #M0_SHMEM_READ_IDX]
	ands	r0, r2			@ in range and frame aligned, whatever the host wrote
	str	r0, [sp, #PS_PUB]
	subs	r0, #M0_FRAME_BYTES
	ands	r0, r2
	str	r0, [sp, #PS_CUR]	@ "playing" the frame before the first one
	movs	r0, #0
	str	r0, [sp, #PS_ACC]
	str	r0, [sp, #PS_WIDX]
	str	r0, [sp, #PS_FRAME]
	str	r0, [sp, #PS_OVERRUNS]
	mvns	r1, r0
	str	r1, [sp, #PS_MIN_CVR]

	mov	r8, r0			@ x_l = x_r = 0
	mov	r9, r0
	movs	r1, #0			@ i2
	movs	r4, #0
	movs	r2, #1
	lsls	r2, r2, #DSM_FS_SHIFT	@ FS
	negs	r0, r2			@ i1_l = -G, G = +FS
	mov	r3, r2			@ i1_r = -G, G = -FS: the idle patterns interleave
	ldr	r6, =SCB_ICSR
	mov	r10, r6
	ldr	r6, =GPIO4_DR_B23_HIGH
	mov	r12, r6
	ldr	r6, =GPIO4_DR_ADDR
	mov	lr, r6
	ldr	r7, =GPIO4_DR_B23_LOW	@ first tick writes both low
	b	.L_sample

	.ltorg

.L_sample:
	STEP				@ 1: stop?
	ldr	r7, [sp, #PS_K_SHMEM]
	ldr	r6, [r7, #M0_SHMEM_CTRL]
	cmp	r6, #M0_CTRL_PLAY
	beq	1f
	b	.L_stop
1:
	STEP_END 1

	STEP				@ 2: plain ticks in this sample
	ldr	r6, [sp, #PS_ACC]
	ldr	r7, [sp, #PS_FRAC]
	adds	r6, r6, r7		@ carry out = one extra tick
	str	r6, [sp, #PS_ACC]
	ldr	r5, [sp, #PS_PLAIN]
	movs	r7, #0			@ (leaves the carry alone)
	adcs	r5, r7
	STEP_END 2

	STEP				@ 3: where is the host writing
	ldr	r7, [sp, #PS_K_SHMEM]
	ldr	r6, [r7, #M0_SHMEM_WRITE_IDX]
	str	r6, [sp, #PS_WIDX]
	STEP_END 3

	STEP				@ 4: next frame, unless the ring is empty
	ldr	r7, [sp, #PS_CUR]
	adds	r7, #M0_FRAME_BYTES
	ldr	r6, [sp, #PS_K_MASK]
	ands	r7, r6
	ldr	r6, [sp, #PS_WIDX]
	cmp	r6, r7
#ifdef M0_PROFILE
	bne	3f
	TRACE_EVENT M0_TRACE_UNDERRUN	@ empty: stay on the current frame
	b	1f
3:	str	r7, [sp, #PS_CUR]
#else
	beq	1f			@ empty: stay on the current frame
	str	r7, [sp, #PS_CUR]
#endif
1:
	STEP_END 4

	STEP				@ 5: fetch it: L in [15:0], R in [31:16]
	ldr	r7, [sp, #PS_CUR]
	ldr	r6, [sp, #PS_K_RING]
	ldr	r6, [r6, r7]
	str	r6, [sp, #PS_FRAME]
	STEP_END 5

	STEP				@ 6: left
	ldr	r6, [sp, #PS_FRAME]
	sxth	r6, r6
	asrs	r7, r6, #3
	subs	r6, r6, r7		@ x -= x >> 3 (7/8 headroom)
	mov	r8, r6
	CLAMP	r1
	STEP_END 6

	STEP				@ 7: right
	ldr	r6, [sp, #PS_FRAME]
	asrs	r6, r6, #16
	asrs	r7, r6, #3
	subs	r6, r6, r7
	mov	r9, r6
	CLAMP	r4
	STEP_END 7

	STEP				@ 8: read_idx = the frame after the current one
	ldr	r6, [sp, #PS_CUR]
	adds	r6, #M0_FRAME_BYTES
	ldr	r7, [sp, #PS_K_MASK]
	ands	r6, r7
	str	r6, [sp, #PS_PUB]
	STEP_END 8

	STEP				@ 9: tell the host
	ldr	r6, [sp, #PS_PUB]
	ldr	r7, [sp, #PS_K_SHMEM]
	str	r6, [r7, #M0_SHMEM_READ_IDX]
	STEP_END 9

#ifdef M0_PROFILE
	STEP				@ 10
	ldr	r6, [sp, #PS_MIN_CVR]
	ldr	r7, [sp, #PS_K_SHMEM]
	str	r6, [r7, #M0_SHMEM_STAT_MIN_CVR]
	STEP_END 10

	STEP				@ 11
	ldr	r6, [sp, #PS_OVERRUNS]
	ldr	r7, [sp, #PS_K_SHMEM]
	str	r6, [r7, #M0_SHMEM_STAT_OVERRUNS]
	STEP_END 11

	STEP				@ 12
	ldr	r6, [sp, #PS_SAMPLE_NO]
	adds	r6, #1
	str	r6, [sp, #PS_SAMPLE_NO]
	STEP_END 12
#endif

.L_plain:				@ r5 plain ticks, at least one
	TICK
#ifdef M0_PROFILE
	mov	r11, r7
	STEP_END M0_TRACE_PLAIN
#else
	mov	r6, lr
#endif
	subs	r5, #1
	bne	.L_plain
	b	.L_sample

.L_stop:
	ldr	r6, [sp, #PS_SAVED_SP]
	mov	sp, r6
	pop	{r4-r7}
	mov	r8, r4
	mov	r9, r5
	mov	r10, r6
	mov	r11, r7
	pop	{r4-r7, pc}

	.ltorg
	.size	m0_play, . - m0_play
