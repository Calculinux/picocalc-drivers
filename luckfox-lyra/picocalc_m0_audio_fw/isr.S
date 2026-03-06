/* SPDX-License-Identifier: GPL-2.0 */
/* Hand-tuned TIMER0_CH5 ISR: load-order scheduling to avoid load-use stalls on Cortex-M0.
 *
 * HIGH-LEVEL BEHAVIOUR:
 *   This ISR runs at ~1 MHz (TIMER0_CH5, DS_PERIOD_TICKS=100). Each tick it:
 *   1. Clears the timer interrupt and advances a phase accumulator (phase += rate).
 *   2. When phase >= DS_RATE, "consumes" one stereo sample from the host ring buffer:
 *      reads next L/R 16-bit sample, updates last_l/last_r, increments read_idx, and
 *      every BATCH samples writes read_idx to shared memory for the host (with DMB).
 *   3. Runs a second-order delta-sigma modulator (DSM) for left and right using
 *      last_l/last_r as input; produces 1-bit outputs out_l, out_r.
 *   4. Writes out_l/out_r to GPIO (GPIO4_B2 = left, GPIO4_B3 = right) for bit-bang PWM.
 *
 * Cycle counts per instruction (Cortex-M0, zero-wait-state RAM):
 *   1: ALU (adds, subs, movs, lsls, lsrs, asrs, orrs, ands), cmp, mov (reg)
 *   2: ldr, str, ldrsh, b/bl (taken), push/pop 1 reg
 *   3: dmb
 *   LDM/STM: 1+N cycles for N registers. Save/restore via r8-r11 + push {lr}/pop {pc}.
 */

.syntax unified
.thumb
.cpu cortex-m0

.section .ramfunc, "ax", %progbits
.align 2
.global TIMER0_CH5_IRQHandler
.thumb_func
.type TIMER0_CH5_IRQHandler, %function

/* ---------------------------------------------------------------------------
 * m0_isr_globs struct offsets. All state and constants at fixed offsets from
 * a single base (r5). Left/right DSM state is contiguous for LDM.
 * --------------------------------------------------------------------------- */
.equ O_TIMER,   0   @ Timer base (e.g. TIMER_INTSTAT) for clearing IRQ
.equ O_GPIO,    4   @ GPIO data register for bit-bang output
.equ O_PHASE,   8   @ Phase accumulator; when >= DS_RATE we consume a sample
.equ O_READ_IDX, 12 @ Ring-buffer read index (bytes; +=4 per stereo sample)
.equ O_BUF_PTR, 16 @ Pointer to host sample buffer (int16_t L, R pairs)
.equ O_CTR,     20 @ Samples consumed in current batch (reset when batch done)
.equ O_I1L,     32 @ Left DSM: first integrator
.equ O_I2L,     36 @ Left DSM: second integrator
.equ O_LAST_L,  40 @ Left: last consumed sample (DSM input)
.equ O_OUTL,    44 @ Left: current 1-bit output (0 or 1)
.equ O_I1R,     48 @ Right DSM: first integrator
.equ O_I2R,     52 @ Right DSM: second integrator
.equ O_LAST_R,  56 @ Right: last consumed sample (DSM input)
.equ O_OUTR,    60 @ Right: current 1-bit output (0 or 1)
.equ O_SHMEM,   64 @ Shared memory where we write read_idx for host
.equ O_RATE,    68 @ Phase increment per tick
.equ O_DSRATE,  72 @ When phase >= this, consume one sample and subtract it
.equ O_MASK,    76 @ Ring buffer size mask (buf_size - 1) for wrapping
.equ O_HALF,    80 @ DSM constant 16384 (half-scale) for integ1
.equ O_FULL,    84 @ DSM constant 32768 (full-scale) for integ2
.equ O_BATCH,   88 @ Samples per batch before writing read_idx to shmem
.equ O_GPIO_MASK, 92 @ GPIO mask to preserve; we OR in (out_l<<10)|(out_r<<11)
.equ SHMEM_READ_IDX_OFF, 12 @ Offset in shmem for read_idx

/* PROLOGUE: save lr and preserve r4-r7 in r8-r11 so we can use r4-r7 without
 * load-use stalls. r5 = &m0_isr_globs for all O_* offsets. */
TIMER0_CH5_IRQHandler:
	push    {lr}                         @ 2  Return address (return via pop {pc})
	mov     r8, r4                       @ 1  Callee-saved; restore in epilogue
	mov     r9, r5                       @ 1
	mov     r10, r6                      @ 1
	mov     r11, r7                      @ 1
	ldr     r5, .L_globs                 @ 2  r5 = base pointer for globs

	/* Clear timer IRQ, then advance phase. phase += rate; if phase >= DS_RATE
	 * we consume one stereo sample and subtract DS_RATE (sample-rate timing). */
	movs    r0, #1                       @ 1  Write 1 to clear IRQ
	ldr     r1, [r5, #O_TIMER]          @ 2  Timer base (e.g. TIMER_INTSTAT)
	str     r0, [r1]                     @ 2  Clear interrupt
	ldr     r1, [r5, #O_PHASE]          @ 2  Current phase
	ldr     r0, [r5, #O_RATE]           @ 2  Phase increment per tick
	adds    r1, r1, r0                   @ 1  phase += rate
	ldr     r0, [r5, #O_DSRATE]         @ 2  Threshold
	cmp     r1, r0                       @ 1  if (phase < DS_RATE) skip consume
	blo     .L_no_consume                @ 2

	/* CONSUME: phase -= DS_RATE, then read next L/R from ring buffer.
	 * Buffer: int16_t left, int16_t right; read_idx in bytes, +=4, wrap with mask. */
	subs    r1, r1, r0                   @ 1  phase -= DS_RATE
	str     r1, [r5, #O_PHASE]           @ 2
	ldr     r1, [r5, #O_READ_IDX]       @ 2  Byte index into buffer
	ldr     r3, [r5, #O_BUF_PTR]        @ 2  Buffer base
	ldr     r0, [r5, #O_MASK]           @ 2  Size mask
	mov     r7, r0                       @ 1  Keep mask in r7 for wrap
	mov     r2, r1                       @ 1
	ands    r2, r0                       @ 1  r2 = read_idx & mask (wrapped offset)
	adds    r4, r3, r2                   @ 1  r4 = &buffer[read_idx] (left sample)
	movs    r3, #0                       @ 1  Offset 0 for ldrsh
	ldrsh   r0, [r4, r3]                @ 2  Load signed left
	str     r0, [r5, #O_LAST_L]         @ 2  last_l = input to left DSM
	adds    r4, r4, #2                   @ 1  Point to right sample
	ldrsh   r0, [r4, r3]                @ 2  Load signed right
	str     r0, [r5, #O_LAST_R]         @ 2  last_r = input to right DSM
	adds    r1, r1, #4                   @ 1  read_idx += 4 (one stereo frame)
	ands    r1, r7                       @ 1  read_idx &= mask (wrap)
	str     r1, [r5, #O_READ_IDX]        @ 2
	/* Every BATCH samples: write read_idx to shmem for host, reset ctr, DMB. */
	ldr     r0, [r5, #O_CTR]            @ 2  Batch counter
	adds    r0, r0, #1                   @ 1  ctr++
	str     r0, [r5, #O_CTR]             @ 2
	ldr     r3, [r5, #O_BATCH]          @ 2
	cmp     r0, r3                       @ 1  if (ctr < batch) skip
	blo     .L_consume_done              @ 2
	movs    r0, #0                       @ 1  ctr = 0
	str     r0, [r5, #O_CTR]             @ 2
	ldr     r2, [r5, #O_SHMEM]          @ 2  Shmem base
	adds    r2, r2, #SHMEM_READ_IDX_OFF @ 1  &shmem->read_idx
	str     r1, [r2]                     @ 2  Notify host of read position
	dmb     sy                           @ 3  Barrier so A55 sees the store
.L_consume_done:
	b       .L_dsm                       @ 2

.L_no_consume:
	str     r1, [r5, #O_PHASE]           @ 2  Store updated phase (no consume)

/* DSM: second-order delta-sigma. integ1 += last + HALF - (out<<15);
 * integ2 += integ1 + FULL - (out_new<<16); out_new = sign(integ2) in {0,1}. */
.L_dsm:
	/* Left channel: load i1l, i2l, last_l, outl in one LDM (base r5+O_I1L, imm8 add). */
	mov     r0, r5                       @ 1
	adds    r0, #O_I1L                   @ 1
	ldm     r0!, {r1, r2, r3, r4}       @ 5  r1=i1l, r2=i2l, r3=last_l, r4=outl
	ldr     r0, [r5, #O_HALF]           @ 2  HALF (16384)
	adds    r1, r1, r3                   @ 1  integ1 += last_l (input)
	adds    r1, r1, r0                   @ 1  integ1 += half
	lsls    r4, r4, #15                  @ 1  outl << 15 (feedback)
	subs    r1, r1, r4                   @ 1  integ1_new
	str     r1, [r5, #O_I1L]             @ 2
	adds    r2, r2, r1                   @ 1  integ2 += integ1_new
	asrs    r4, r2, #31                  @ 1  sign(integ2): -1 or 0
	adds    r4, r4, #1                   @ 1  out_new = 0 or 1 (1 if integ2>=0)
	ldr     r0, [r5, #O_FULL]           @ 2  FULL (32768)
	adds    r2, r2, r0                   @ 1  integ2 += full
	lsls    r0, r4, #16                  @ 1  out_new << 16 (feedback)
	subs    r2, r2, r0                   @ 1  integ2_new
	str     r2, [r5, #O_I2L]             @ 2
	str     r4, [r5, #O_OUTL]             @ 2
	mov     r7, r4                       @ 1  Save out_l for GPIO (right DSM overwrites r4)

	/* Right channel: same DSM math, state at O_I1R..O_OUTR. */
	mov     r0, r5                       @ 1
	adds    r0, #O_I1R                   @ 1
	ldm     r0!, {r1, r2, r3, r4}       @ 5  r1=i1r, r2=i2r, r3=last_r, r4=outr
	ldr     r0, [r5, #O_HALF]           @ 2
	adds    r1, r1, r3                   @ 1  integ1 += last_r
	adds    r1, r1, r0                   @ 1  integ1 += half
	lsls    r4, r4, #15                  @ 1  outr << 15
	subs    r1, r1, r4                   @ 1  integ1_new
	str     r1, [r5, #O_I1R]             @ 2
	adds    r2, r2, r1                   @ 1  integ2 += integ1_new
	asrs    r4, r2, #31                  @ 1  sign(integ2): -1 or 0
	adds    r4, r4, #1                   @ 1  out_new = 0 or 1
	ldr     r0, [r5, #O_FULL]           @ 2
	adds    r2, r2, r0                   @ 1  integ2 += full
	lsls    r0, r4, #16                  @ 1
	subs    r2, r2, r0                   @ 1  integ2_new
	str     r2, [r5, #O_I2R]             @ 2
	str     r4, [r5, #O_OUTR]             @ 2  r4 = out_r for GPIO below

	/* GPIO: drive GPIO4_B2 (left) and GPIO4_B3 (right). O_GPIO_MASK preserves
	 * other bits; we OR in (out_l<<10) | (out_r<<11) per RK3506 GPIO4 DR layout. */
	mov     r1, r7                       @ 1  out_l (saved above)
	ldr     r2, [r5, #O_GPIO_MASK]      @ 2  Base mask
	lsls    r1, r1, #10                  @ 1  out_l << 10 (B2)
	lsls    r0, r4, #11                  @ 1  out_r << 11 (B3)
	orrs    r1, r1, r0                   @ 1  Combined output bits
	orrs    r2, r2, r1                   @ 1  Final GPIO word
	ldr     r0, [r5, #O_GPIO]           @ 2  GPIO DR address
	str     r2, [r0]                     @ 2  Write (atomic update of B2/B3)

	/* EPILOGUE: restore r4-r7 from r8-r11, return via pop {pc}. */
	mov     r4, r8                       @ 1
	mov     r5, r9                       @ 1
	mov     r6, r10                      @ 1
	mov     r7, r11                      @ 1
	pop     {pc}                         @ 2  Return from interrupt

.L_globs:
	.word   m0_isr_globs
.size TIMER0_CH5_IRQHandler, . - TIMER0_CH5_IRQHandler
