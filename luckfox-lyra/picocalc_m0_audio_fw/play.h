/* SPDX-License-Identifier: GPL-2.0 */
/* m0_play() and the state it keeps in memory (play.S). Byte offsets into
 * m0_play_state; main.c fills in the sample timing before calling m0_play().
 * While m0_play() runs, SP points at this block. */

#ifndef M0_PLAY_H
#define M0_PLAY_H

#define PS_ACC        0   /* fractional ticks accumulated (2^-32 of a tick) */
#define PS_FRAC       4   /* set by caller: added to PS_ACC once per sample */
#define PS_PLAIN      8   /* set by caller: plain ticks per sample before any carry */
#define PS_CUR        12  /* ring offset of the frame being played */
#define PS_WIDX       16  /* snapshot of the host's write_idx */
#define PS_FRAME      20  /* that frame: left in [15:0], right in [31:16] */
#define PS_PUB        24  /* read_idx to publish: the next frame to fetch */
#define PS_SAVED_SP   28
#define PS_MIN_CVR    32  /* PROFILE: lowest SysTick count seen at end of work */
#define PS_OVERRUNS   36  /* PROFILE: ticks that ran into the next one */
/* Constants kept here so the steps can load them SP-relative */
#define PS_K_SHMEM    40  /* M0_SHMEM_ADDR */
#define PS_K_RING     44  /* address of ring byte 0 */
#define PS_K_MASK     48  /* ring index mask, frame aligned */
#define PS_K_CLAMP    52  /* largest i2 the clamp allows */
#define PS_K_CVR      56  /* &SYST_CVR */
#define PS_SAMPLE_NO  60  /* PROFILE: samples since the stream started */
#define PS_EVENTS     64  /* PROFILE: events logged so far */
#define PS_K_TRACE    68  /* PROFILE: M0_TRACE_ADDR */
#define PS_SHIFT      72  /* set by caller: the modulator works in units of 2^-shift LSB */
#define PS_DX_MASK    76  /* set by caller: ~0 to interpolate, 0 to hold each sample */
#define PS_K_ICSR     80  /* &SCB_ICSR */
#define PS_K_GPIO     84  /* GPIO4 data register */
#define PS_CLAMP_SH   88  /* i2 is in range iff i2 >> this is 0 or -1 */
/* Per channel: the sample just fetched and the one before it (7/8 scaled),
 * and what the next stretch of ticks starts from and steps by */
#define PS_NL         92
#define PS_VL         96
#define PS_DXL        100
#define PS_XL         104
#define PS_NR         108
#define PS_VR         112
#define PS_DXR        116
#define PS_XR         120
#define PS_NEXT       124 /* ring offset of the frame after PS_CUR */
#define PS_FADDR      128 /* address of the frame being played */
/* m0_play_comp only */
#define PS_C0         132 /* SysTick count straight after an on-time pin write */
#define PS_K_HIGH     136 /* GPIO word, both high */
#define PS_DSUM       140 /* the lateness of every plain tick's pin write, added up */
#define PS_SIZE       144

/*
 * Ticks at the start of every sample that each carry one step of the
 * per-sample work (play.S); the rest of the sample is plain ticks. The first
 * M0_BUS_STEPS (and the M0_PROFILE_STEPS after them) are the only ones that
 * touch the shared SRAM; the host times its ring writes by that.
 */
#define M0_BUS_STEPS  8
#ifdef M0_PROFILE
#define M0_PROFILE_STEPS 3
#else
#define M0_PROFILE_STEPS 0
#endif
#define M0_STEP_TICKS (19 + M0_PROFILE_STEPS)

/* The input moves 2^-shift of the way to the next sample per tick, so a
 * sample may last at most 2^shift ticks; the state has room for this much. */
#define M0_MAX_SHIFT  6

/* m0_play_comp(): a tick is exactly this many core cycles (2^7), the shift is
 * M0_MAX_SHIFT, and the count of an on-time pin write is taken from this many
 * ticks before anything is played. */
#define M0_COMP_LOG2_CYCLES 7
#define M0_COMP_CYCLES      (1 << M0_COMP_LOG2_CYCLES)
#define M0_COMP_CAL_TICKS   16

#ifndef __ASSEMBLER__
#include <stdint.h>

extern uint32_t m0_play_state[PS_SIZE / 4];

/* SysTick running at the tick rate, PRIMASK set, shmem header valid, PS_FRAC,
 * PS_PLAIN, PS_SHIFT and PS_DX_MASK set.
 * Returns when shmem->ctrl leaves M0_CTRL_PLAY, pins as the last tick left them. */
void m0_play(void);
/* The same, timing each pin write and correcting for late ones (play.S). Also
 * needs: a tick of M0_COMP_CYCLES, PS_SHIFT = M0_MAX_SHIFT, the M0's bus
 * writes not bufferable. Not in PROFILE builds. */
void m0_play_comp(void);
#endif

#endif /* M0_PLAY_H */
