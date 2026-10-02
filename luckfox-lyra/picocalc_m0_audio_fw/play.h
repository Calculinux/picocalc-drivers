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
/* m0_play_comp only */
#define PS_W          124 /* the GPIO word written on the previous tick */
#define PS_WN         128 /* scratch: the one just written */
#define PS_M          132 /* scratch: what a late edge is put back as */
#define PS_CYCLES     136 /* set by caller: core cycles per tick */
#define PS_K_COMP     140 /* set by caller: 2 * FS / PS_CYCLES, FS as scaled by PS_SHIFT */
#define PS_K_HIGH     144 /* GPIO word, both high */
#define PS_DXRM       148 /* dx_r as it is in lr */
#define PS_SIZE       152

/*
 * Ticks at the start of every sample that each carry one step of the
 * per-sample work (play.S); the rest of the sample is plain ticks. The first
 * M0_BUS_STEPS (and the M0_PROFILE_STEPS after them) are the only ones that
 * touch the shared SRAM; the host times its ring writes by that.
 */
#define M0_BUS_STEPS  6
#ifdef M0_PROFILE
#define M0_PROFILE_STEPS 3
#else
#define M0_PROFILE_STEPS 0
#endif
#define M0_STEP_TICKS (15 + M0_PROFILE_STEPS)

/* The input moves 2^-shift of the way to the next sample per tick, so a
 * sample may last at most 2^shift ticks; the state has room for this much. */
#define M0_MAX_SHIFT  6

/* m0_play_comp() takes the SysTick count of an on-time pin write from this
 * many ticks before it plays anything */
#define M0_COMP_CAL_TICKS 16

#ifndef __ASSEMBLER__
#include <stdint.h>

extern uint32_t m0_play_state[PS_SIZE / 4];

/* SysTick running at the tick rate, PRIMASK set, shmem header valid, PS_FRAC,
 * PS_PLAIN, PS_SHIFT and PS_DX_MASK set; for m0_play_comp() also PS_CYCLES
 * and PS_K_COMP, and the M0's bus writes not bufferable (play.S).
 * Returns when shmem->ctrl leaves M0_CTRL_PLAY, pins as the last tick left them. */
void m0_play(void);
void m0_play_comp(void);
#endif

#endif /* M0_PLAY_H */
