/* SPDX-License-Identifier: GPL-2.0 */
/* M0 delta-sigma playback loop: one output bit per channel per SysTick tick.
 *
 * WHY A LOOP AND NOT AN INTERRUPT HANDLER
 *   Exception entry and exit cost 32 cycles on a Cortex-M0, and a handler has
 *   to load and store every piece of state it touches. The M0 does nothing
 *   else while playing, so m0_play() owns every register, SP and LR included,
 *   and keeps the modulator state in them.
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
 *   The GPIO block is clocked at half the core clock, so a tick must be an
 *   even number of core cycles or every other edge comes out a cycle late
 *   (the host sees to that).
 *
 * ONE TICK (the TICK macro)
 *      wfi
 *      GPIO <- word computed on the previous tick (B2 = left, B3 = right)
 *      clear SysTick's pending bit
 *      both modulators, next GPIO word
 *
 * THE MODULATOR (per channel; G = +FS for an output 1, -FS - 1 for a 0)
 *      i1 += x                      @ i1 already has the previous -G in it
 *      x  += dx                     @ on towards the next sample
 *      y   = i2 + i1
 *      s   = y >> 31                @ 0: output 1, -1: output 0
 *      G   = FS ^ s
 *      i2  = y - G
 *      i1 -= G
 *   which is the standard MOD2: out = x + (1 - z^-1)^2 * e (unity signal gain,
 *   second-order noise shaping). Both stages need the same +/-FS feedback.
 *   A single-bit MOD2 is only well behaved short of full scale, so samples are
 *   scaled by 7/8 as they are fetched, and i2 is clamped so that no input
 *   (full-scale noise, Nyquist-rate squares) can run the state into int32
 *   wrap. The clamp runs once per sample, not per tick. check_dsm.c mirrors
 *   this arithmetic bit-exactly; run `make check` after any change here.
 *
 * BETWEEN SAMPLES
 *   Everything is in units of 2^-shift of an input LSB (PS_SHIFT; FS is
 *   32768 << shift), where 2^shift is at least the number of ticks in a
 *   sample. Each sample x starts at the previous sample's value and dx is the
 *   difference to the new one, in LSBs: after n ticks x has gone n / 2^shift
 *   of the way, with no multiply and no rounding. It is set to the exact
 *   value again every sample, so nothing accumulates; what is left of the way
 *   when the sample ends (none if it lasts 2^shift ticks) is taken in one
 *   step. The output is therefore one sample late. With PS_DX_MASK zero dx
 *   stays zero and each sample is held; main.c then sets the shift to zero
 *   too (a held full-scale square drives i2 about 30 times further than a
 *   ramped one, and the headroom is wanted).
 *
 * ONE SAMPLE
 *   The per-sample work is cut into M0_STEP_TICKS steps of at most a dozen
 *   cycles, and each of the first M0_STEP_TICKS ticks of a sample carries one,
 *   as straight-line code: no tick is much longer than a plain one and there
 *   is nothing to dispatch. Then come the plain ticks, in a counted loop.
 *   The steps that use the shared SRAM come first, so the host knows when
 *   it is not in use:
 *      1  stop if ctrl says so                                  (SRAM)
 *      2  snapshot the host's write_idx                         (SRAM)
 *      3  which frame would be next
 *      4  move on to it, unless that is where the host is writing
 *      5  its address
 *      6  fetch it                                              (SRAM)
 *      7  work out read_idx
 *      8  publish read_idx                                      (SRAM)
 *     (9-11 in PROFILE builds: publish the profile figures, count the sample)
 *   and then, numbered on from there:
 *      how many plain ticks this sample has
 *      left:  the new sample
 *      left:  where x starts (the previous sample)
 *      left:  the difference between the two
 *      right: the same three
 *      left:  x and dx into their registers
 *      right: the same
 *      clamp left, clamp right
 *   Left and right change one tick apart. On an underrun step 4 stays on the
 *   same frame, step 6 fetches it again and the output holds; at the start of
 *   a stream that frame is the one before read_idx, which the host zeroes.
 *
 * SAMPLE LENGTH
 *   A sample lasts PS_PLAIN + M0_STEP_TICKS ticks, plus one whenever the
 *   32-bit fraction PS_FRAC, added to PS_ACC each sample, carries out.
 *   main.c sets both from the host's figures (shmem.h).
 *
 * COST
 *   Do not count cycles from the listing alone: in bus mode this image runs
 *   at about 2.5 cycles per instruction (1 as TCM). Build with PROFILE=1:
 *   every tick then records the lowest SysTick count seen at the end of its
 *   work and whether it ran into the next tick, and the host driver reports
 *   both when a stream ends. That costs about 17 cycles per tick, 9 of which
 *   are included in the figure. It also logs every overrun and every
 *   empty-ring event with the sample number it happened in (shmem.h,
 *   M0_TRACE_ADDR); m0trace reads the log. docs/m0-audio.md has the measured
 *   figures. A tick that overruns only delays its own edge and the next tick
 *   starts late; none are lost unless the work exceeds two ticks.
 *
 * REGISTERS
 *   r0 i1_l   r1 i2_l           r8  x_l    r10 dx_l    r11 scratch / parked GPIO word
 *   r3 i1_r   r4 i2_r           r9  x_r    lr  dx_r    r12 GPIO word, both high
 *   r2 FS     r5 plain ticks    r6, r7 scratch         sp  m0_play_state
 *   Entering a tick, r7 holds the GPIO word to write and r6 the GPIO address.
 *
 * LATE PIN WRITES: m0_play_comp()
 *   The M0's bus writes wait while Linux's SPI controllers (the display's
 *   pixel data above all) or its GPIO writes have the peripheral bus:
 *   measured, a quarter of the pin writes are up to 0.4 us late while the
 *   display is redrawn, and every late edge is an error in the output.
 *   m0_play_comp() is the same loop with the pin write made blocking (main.c
 *   turns "bufferable" off for it), so that the SysTick count read straight
 *   after it says when the pin changed:
 *      d      = (count when on time - count) mod tick   @ cycles late
 *      delta  = d - previous d      @ the level that just ended lasted a
 *                                   @ tick + delta
 *      m      = FS * delta / tick
 *      i1, i2 -= m if that level was 1, += m if it was 0, per channel
 *   i.e. the feedback already taken for that bit is brought to what the bit
 *   really delivered, and the error is shaped out of the audio band like the
 *   quantisation error is (in simulation, a display redraw: from -24 dBFS of
 *   noise to -60, or to -72 with the display's DMA bursts shortened). It is
 *   done on every tick, late or not, without a branch that depends on d: a
 *   tick always costs the same and a late one cannot make the next late.
 *   Requires a tick of exactly 128 core cycles and a shift of 6 (the two
 *   make "modulo the tick" and "FS * delta / tick" shifts by a constant):
 *   2.93 MHz at a 375 MHz core clock, where the blocking write takes 49 of
 *   the 128 cycles. SysTick's pending bit is cleared before the write, so
 *   that a write that lands after the next tick cannot wipe that tick out.
 *   The count that means "on time" is the highest of the first ticks of each
 *   stream. r12 holds the previous d plus the word written then; the tick
 *   starts with its word in r11, not r7. Not in PROFILE builds.
 *
 * LAYOUT
 *   The loop is longer than a Thumb branch reaches in a PROFILE build, so the
 *   plain-tick loop sits in the middle of it: steps 1 onwards, a hop over the
 *   plain loop, the remaining steps, back to the plain loop, back to step 1.
 */

#include "rk3506_regs.h"
#include "shmem.h"
#include "play.h"

	.syntax unified
	.cpu cortex-m0
	.thumb

	.equ	GPIO4_DR_ADDR, GPIO4_BASE + GPIO_DR_L
	.equ	RING_IDX_MASK, M0_RING_BYTES - M0_FRAME_BYTES
	.equ	PENDST_TO_SIGN, 31 - ICSR_PENDSTSET_BIT
	.equ	S, M0_BUS_STEPS + M0_PROFILE_STEPS	@ number of the last SRAM step
	.equ	COMP_LSL, DSM_FS_SHIFT + M0_MAX_SHIFT - M0_COMP_LOG2_CYCLES
	.equ	COMP_MOD, 32 - M0_COMP_LOG2_CYCLES

/* For check_play.py, which has to know how this build schedules a sample */
	.global	m0_step_ticks
	.equ	m0_step_ticks, M0_STEP_TICKS

	.section .bss.m0_play_state, "aw", %nobits
	.align	3
	.global	m0_play_state
m0_play_state:
	.space	PS_SIZE

/* Both modulators: the bit each channel puts out next, as r7 = 0 or
 * -(1 << bit) per channel summed (the caller adds the both-high word). r11 is
 * used on the way. */
	.macro	MODULATE
	add	r0, r8			@ LEFT  i1 += x
	add	r8, r10			@       x += dx
	adds	r1, r1, r0		@       y = i2 + i1
	asrs	r7, r1, #31		@       s = 0 (bit 1) or -1 (bit 0)
	mov	r6, r7
	eors	r6, r2			@       G = FS ^ s
	subs	r1, r1, r6		@       i2 = y - G
	subs	r0, r0, r6		@       i1 -= G
	lsls	r7, r7, #GPIO4_B2_BIT	@       0, or -(1 << 10) to clear B2
	mov	r11, r7

	add	r3, r9			@ RIGHT, same with r3/r4, r9/lr and B3
	add	r9, lr
	adds	r4, r4, r3
	asrs	r7, r4, #31
	mov	r6, r7
	eors	r6, r2
	subs	r4, r4, r6
	subs	r3, r3, r6
	lsls	r7, r7, #GPIO4_B3_BIT
	add	r7, r11
	.endm

/* m0_play_comp: tick taken, pin write, and how late it was. Leaves d in r6. */
	.macro	WRITE_TIMED
	ldr	r7, [sp, #PS_K_ICSR]
	movs	r6, #1
	lsls	r6, r6, #ICSR_PENDSTCLR_BIT
	str	r6, [r7]		@ tick taken: clear SysTick's pending bit
	ldr	r6, [sp, #PS_K_GPIO]
	mov	r7, r11
	str	r7, [r6]		@ OUTPUT, and wait until the pin has changed
	ldr	r6, [sp, #PS_K_CVR]
	ldr	r6, [r6]		@ when was that
	ldr	r7, [sp, #PS_C0]
	subs	r6, r7, r6
	lsls	r6, r6, #COMP_MOD
	lsrs	r6, r6, #COMP_MOD	@ d = cycles late, modulo the tick
	.endm

/* Wait for the tick, put out the bits, run both modulators.
 * comp 0: enters with the GPIO word in r7 and its address in r6, leaves the
 *         next word in r7; r6 is free.
 * comp 1: enters and leaves with the word in r11; r6, r7 are free. stats:
 *         also add the lateness to the total. */
	.macro	TICK comp, stats=0
	wfi
	.if	\comp
	WRITE_TIMED
	.if	\stats
	ldr	r7, [sp, #PS_DSUM]
	adds	r7, r7, r6
	str	r7, [sp, #PS_DSUM]
	.endif
	mov	r7, r12			@ the previous d + the word written then
	mov	r12, r6
	add	r12, r11		@ this d + this word, for the next tick
	subs	r6, r6, r7
	sxtb	r6, r6			@ delta = d - previous d (the words are bits 10 up)
	lsls	r6, r6, #COMP_LSL	@ m = FS * delta / tick
	lsls	r7, r7, #31 - GPIO4_B2_BIT	@ the levels that just ended: N = left, C = right
	bcs	2f
	bmi	1f
	adds	r0, r0, r6		@ left 0, right 0
	adds	r1, r1, r6
	adds	r3, r3, r6
	adds	r4, r4, r6
	b	9f
1:	subs	r0, r0, r6		@ left 1, right 0
	subs	r1, r1, r6
	adds	r3, r3, r6
	adds	r4, r4, r6
	b	9f
2:	bmi	3f
	adds	r0, r0, r6		@ left 0, right 1
	adds	r1, r1, r6
	subs	r3, r3, r6
	subs	r4, r4, r6
	b	9f
3:	subs	r0, r0, r6		@ left 1, right 1
	subs	r1, r1, r6
	subs	r3, r3, r6
	subs	r4, r4, r6
9:
	MODULATE
	ldr	r6, [sp, #PS_K_HIGH]
	adds	r7, r7, r6		@ both-high word minus the bits that are 0
	mov	r11, r7
	.else
	str	r7, [r6]		@ OUTPUT: bits from the previous tick
	ldr	r7, [sp, #PS_K_ICSR]
	movs	r6, #1
	lsls	r6, r6, #ICSR_PENDSTCLR_BIT
	str	r6, [r7]		@ tick taken: clear SysTick's pending bit
	MODULATE
	add	r7, r12			@ both-high word minus the bits that are 0
	.endif
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
	adds	r7, #(\code)
	str	r7, [r6, #4]
	.endm

/* End of a tick's work, PROFILE builds: already pending means the tick
 * overran; otherwise remember the lowest SysTick count, i.e. the longest
 * tick so far. Uses r6, r7. */
	.macro	PROFILE_TICK code
#ifdef M0_PROFILE
	ldr	r7, [sp, #PS_K_ICSR]
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
	.macro	STEP comp
	TICK	\comp
	.if	\comp == 0
	mov	r11, r7			@ park the GPIO word
	.endif
	.endm

	.macro	STEP_END comp, code
	PROFILE_TICK \code
	.if	\comp == 0
	mov	r7, r11
	ldr	r6, [sp, #PS_K_GPIO]
	.endif
	.endm

/* Saturate an i2 that has left the clamp window to +MAX or -MAX-1.
 * Uses r6, r7. */
	.macro	CLAMP reg
	ldr	r7, [sp, #PS_CLAMP_SH]
	mov	r6, \reg
	asrs	r6, r7			@ in range iff this is 0 or -1
	adds	r6, #1
	lsrs	r6, r6, #1
	beq	1f
	asrs	\reg, \reg, #31
	ldr	r6, [sp, #PS_K_CLAMP]
	eors	\reg, r6
1:
	.endm

/* The three steps a new sample takes, per channel. r6 on entry to the first:
 * the 16-bit sample, sign-extended. */
	.macro	SAMPLE_NEW n			@ scale it: PS_N
	asrs	r7, r6, #3
	subs	r6, r6, r7		@ x -= x >> 3 (7/8 headroom)
	str	r6, [sp, #\n]
	.endm

	.macro	SAMPLE_START v, x		@ PS_X = the previous one, in modulator units
	ldr	r7, [sp, #\v]
	ldr	r6, [sp, #PS_SHIFT]
	lsls	r7, r6
	str	r7, [sp, #\x]
	.endm

	.macro	SAMPLE_STEP n, v, dx		@ PS_DX = new - previous; new becomes previous
	ldr	r6, [sp, #\n]
	ldr	r7, [sp, #\v]
	str	r6, [sp, #\v]
	subs	r6, r6, r7
	str	r6, [sp, #\dx]
	.endm

	.macro	M0_PLAY name, comp
	.section .ramfunc, "ax", %progbits
	.align	2
	.global	\name
	.thumb_func
	.type	\name, %function

\name:
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
	ldr	r0, =SYST_CVR
	str	r0, [sp, #PS_K_CVR]
	ldr	r0, =SCB_ICSR
	str	r0, [sp, #PS_K_ICSR]
	ldr	r0, =GPIO4_DR_ADDR
	str	r0, [sp, #PS_K_GPIO]
#ifdef M0_PROFILE
	ldr	r0, =M0_TRACE_ADDR
	str	r0, [sp, #PS_K_TRACE]
	movs	r3, #0
	str	r3, [r0]		@ empty log
	str	r3, [sp, #PS_EVENTS]
	str	r3, [sp, #PS_SAMPLE_NO]
#endif

	.if	\comp
	/* The SysTick count straight after an undisturbed pin write: the
	 * highest of a few, since a disturbed one can only come later. */
	ldr	r0, =GPIO4_DR_B23_HIGH
	str	r0, [sp, #PS_K_HIGH]
	ldr	r0, =GPIO4_DR_B23_LOW
	mov	r11, r0
	movs	r0, #0
	str	r0, [sp, #PS_C0]
	str	r0, [sp, #PS_DSUM]
	movs	r5, #M0_COMP_CAL_TICKS
	movs	r4, #0
10:	wfi
	WRITE_TIMED			@ d = -count, modulo the tick, PS_C0 being 0
	negs	r6, r6
	lsls	r6, r6, #COMP_MOD
	lsrs	r6, r6, #COMP_MOD	@ the count
	cmp	r6, r4
	bls	11f
	mov	r4, r6
11:	subs	r5, #1
	bne	10b
	str	r4, [sp, #PS_C0]
	mov	r12, r11		@ no lateness yet, and the word on the pins
	ldr	r1, [sp, #PS_K_SHMEM]
	ldr	r2, [sp, #PS_K_MASK]
	.endif

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
	str	r0, [sp, #PS_NL]
	str	r0, [sp, #PS_VL]
	str	r0, [sp, #PS_DXL]
	str	r0, [sp, #PS_XL]
	str	r0, [sp, #PS_NR]
	str	r0, [sp, #PS_VR]
	str	r0, [sp, #PS_DXR]
	str	r0, [sp, #PS_XR]
	mvns	r1, r0
	str	r1, [sp, #PS_MIN_CVR]

	mov	r8, r0			@ x = dx = 0
	mov	r9, r0
	mov	r10, r0
	mov	lr, r0
	ldr	r6, [sp, #PS_SHIFT]
	movs	r2, #1
	adds	r6, #DSM_CLAMP_SHIFT
	str	r6, [sp, #PS_CLAMP_SH]
	mov	r1, r2
	lsls	r1, r6
	subs	r1, #1
	str	r1, [sp, #PS_K_CLAMP]	@ (1 << (clamp shift + shift)) - 1
	subs	r6, #DSM_CLAMP_SHIFT - DSM_FS_SHIFT
	lsls	r2, r6			@ FS = 32768 << shift
	movs	r1, #0			@ i2
	movs	r4, #0
	negs	r0, r2			@ i1_l = -G, G = +FS
	mov	r3, r2			@ i1_r = -G, G = -FS: the idle patterns interleave
	.if	\comp == 0
	ldr	r6, =GPIO4_DR_B23_HIGH
	mov	r12, r6
	ldr	r6, [sp, #PS_K_GPIO]
	ldr	r7, =GPIO4_DR_B23_LOW	@ first tick writes both low
	.endif
	b	.L_sample_\comp

.L_stop_\comp:
	ldr	r6, [sp, #PS_SAVED_SP]
	mov	sp, r6
	pop	{r4-r7}
	mov	r8, r4
	mov	r9, r5
	mov	r10, r6
	mov	r11, r7
	pop	{r4-r7, pc}

	.ltorg

.L_sample_\comp:
	STEP	\comp			@ 1: stop?
	ldr	r7, [sp, #PS_K_SHMEM]
	ldr	r6, [r7, #M0_SHMEM_CTRL]
	cmp	r6, #M0_CTRL_PLAY
	bne	.L_stop_\comp
	STEP_END \comp, 1

	STEP	\comp			@ 2: where is the host writing
	ldr	r7, [sp, #PS_K_SHMEM]
	ldr	r6, [r7, #M0_SHMEM_WRITE_IDX]
	str	r6, [sp, #PS_WIDX]
	STEP_END \comp, 2

	STEP	\comp			@ 3: the frame after the current one
	ldr	r6, [sp, #PS_CUR]
	adds	r6, #M0_FRAME_BYTES
	ldr	r7, [sp, #PS_K_MASK]
	ands	r6, r7
	str	r6, [sp, #PS_NEXT]
	STEP_END \comp, 3

	STEP	\comp			@ 4: move on to it, unless the ring is empty
	ldr	r6, [sp, #PS_NEXT]
	ldr	r7, [sp, #PS_WIDX]
	cmp	r6, r7
#ifdef M0_PROFILE
	bne	3f
	TRACE_EVENT M0_TRACE_UNDERRUN	@ empty: stay on the current frame
	b	1f
3:	str	r6, [sp, #PS_CUR]
#else
	beq	1f			@ empty: stay on the current frame
	str	r6, [sp, #PS_CUR]
#endif
1:
	STEP_END \comp, 4

	STEP	\comp			@ 5: where the current frame is
	ldr	r6, [sp, #PS_CUR]
	ldr	r7, [sp, #PS_K_RING]
	adds	r6, r6, r7
	str	r6, [sp, #PS_FADDR]
	STEP_END \comp, 5

	STEP	\comp			@ 6: fetch it: L in [15:0], R in [31:16]
	ldr	r6, [sp, #PS_FADDR]
	ldr	r6, [r6]
	str	r6, [sp, #PS_FRAME]
	STEP_END \comp, 6

	STEP	\comp			@ 7: read_idx = the frame after the current one
	ldr	r6, [sp, #PS_CUR]
	adds	r6, #M0_FRAME_BYTES
	ldr	r7, [sp, #PS_K_MASK]
	ands	r6, r7
	str	r6, [sp, #PS_PUB]
	STEP_END \comp, 7

	STEP	\comp			@ 8: tell the host
	ldr	r6, [sp, #PS_PUB]
	ldr	r7, [sp, #PS_K_SHMEM]
	str	r6, [r7, #M0_SHMEM_READ_IDX]
	STEP_END \comp, 8

#ifdef M0_PROFILE
	STEP	\comp			@ 9
	ldr	r6, [sp, #PS_MIN_CVR]
	ldr	r7, [sp, #PS_K_SHMEM]
	str	r6, [r7, #M0_SHMEM_STAT_MIN_CVR]
	STEP_END \comp, 9

	STEP	\comp			@ 10
	ldr	r6, [sp, #PS_OVERRUNS]
	ldr	r7, [sp, #PS_K_SHMEM]
	str	r6, [r7, #M0_SHMEM_STAT_OVERRUNS]
	STEP_END \comp, 10

	STEP	\comp			@ 11
	ldr	r6, [sp, #PS_SAMPLE_NO]
	adds	r6, #1
	str	r6, [sp, #PS_SAMPLE_NO]
	STEP_END \comp, 11
#endif
	/* No more SRAM accesses in this sample. */

	STEP	\comp			@ S+1: plain ticks in this sample
	ldr	r6, [sp, #PS_ACC]
	ldr	r7, [sp, #PS_FRAC]
	adds	r6, r6, r7		@ carry out = one extra tick
	str	r6, [sp, #PS_ACC]
	ldr	r5, [sp, #PS_PLAIN]
	movs	r7, #0			@ (leaves the carry alone)
	adcs	r5, r7
	STEP_END \comp, S + 1

	STEP	\comp			@ S+2: left sample
	ldr	r6, [sp, #PS_FRAME]
	sxth	r6, r6
	SAMPLE_NEW PS_NL
	STEP_END \comp, S + 2
	b	.L_steps2_\comp

.L_plain_\comp:				@ r5 plain ticks, at least one
	TICK	\comp, 1
#ifdef M0_PROFILE
	mov	r11, r7
	STEP_END \comp, M0_TRACE_PLAIN
#else
	.if	\comp == 0
	ldr	r6, [sp, #PS_K_GPIO]
	.endif
#endif
	subs	r5, #1
	bne	.L_plain_\comp
	b	.L_sample_\comp

.L_steps2_\comp:
	STEP	\comp			@ S+3
	SAMPLE_START PS_VL, PS_XL
	STEP_END \comp, S + 3

	STEP	\comp			@ S+4
	SAMPLE_STEP PS_NL, PS_VL, PS_DXL
	STEP_END \comp, S + 4

	STEP	\comp			@ S+5: right sample
	ldr	r6, [sp, #PS_FRAME]
	asrs	r6, r6, #16
	SAMPLE_NEW PS_NR
	STEP_END \comp, S + 5

	STEP	\comp			@ S+6
	SAMPLE_START PS_VR, PS_XR
	STEP_END \comp, S + 6

	STEP	\comp			@ S+7
	SAMPLE_STEP PS_NR, PS_VR, PS_DXR
	STEP_END \comp, S + 7

	STEP	\comp			@ S+8: left starts on its way to the new sample
	ldr	r6, [sp, #PS_XL]
	mov	r8, r6
	ldr	r6, [sp, #PS_DXL]
	ldr	r7, [sp, #PS_DX_MASK]
	ands	r6, r7
	mov	r10, r6
	STEP_END \comp, S + 8

	STEP	\comp			@ S+9: right
	ldr	r6, [sp, #PS_XR]
	mov	r9, r6
	ldr	r6, [sp, #PS_DXR]
	ldr	r7, [sp, #PS_DX_MASK]
	ands	r6, r7
	mov	lr, r6
	STEP_END \comp, S + 9

	STEP	\comp			@ S+10
	CLAMP	r1
	STEP_END \comp, S + 10

	STEP	\comp			@ S+11
	CLAMP	r4
	STEP_END \comp, S + 11
	b	.L_plain_\comp

	.ltorg
	.size	\name, . - \name
	.endm

	M0_PLAY	m0_play, 0
#ifndef M0_PROFILE
	M0_PLAY	m0_play_comp, 1
#endif
