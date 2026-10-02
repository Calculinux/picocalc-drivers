/* SPDX-License-Identifier: GPL-2.0 */
/* m0_play() and the state it keeps in memory (play.S). Byte offsets into
 * m0_play_state; main.c fills in the sample timing before calling m0_play().
 * While m0_play() runs, SP points at this block. */

#ifndef M0_PLAY_H
#define M0_PLAY_H

#define PS_ACC        0   /* fractional ticks accumulated (2^-32 of a tick) */
#define PS_FRAC       4   /* set by main.c: added to PS_ACC once per sample */
#define PS_PLAIN      8   /* set by main.c: plain ticks per sample before any carry */
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
#define PS_SIZE       72

/* Ticks at the start of every sample that each carry one step of the
 * per-sample work (play.S); the rest of the sample is plain ticks. */
#ifdef M0_PROFILE
#define M0_STEP_TICKS 12
#else
#define M0_STEP_TICKS 9
#endif

#ifndef __ASSEMBLER__
#include <stdint.h>

extern uint32_t m0_play_state[PS_SIZE / 4];

/* SysTick running at the tick rate, PRIMASK set, shmem header valid.
 * Returns when shmem->ctrl leaves M0_CTRL_PLAY, pins as the last tick left them. */
void m0_play(void);
#endif

#endif /* M0_PLAY_H */
