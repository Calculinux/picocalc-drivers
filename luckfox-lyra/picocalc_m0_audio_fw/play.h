/* SPDX-License-Identifier: GPL-2.0 */
/* m0_play() and the state it keeps in memory (play.S). Word offsets into
 * m0_play_state; main.c fills in the tick timing before calling m0_play(). */

#ifndef M0_PLAY_H
#define M0_PLAY_H

#define PS_NEXT_L      0   /* prefetched left sample, already scaled */
#define PS_NEXT_R      4
#define PS_NEXT_TICKS  8   /* how many ticks that sample lasts */
#define PS_READ_IDX    12  /* ring read index, bytes */
#define PS_ERR         16  /* accumulated remainder (Bresenham) */
#define PS_BATCH       20  /* samples since read_idx was published */
#define PS_FETCHED     24  /* nonzero: this sample's frame came out of the ring */
#define PS_TICKS_BASE  28  /* set by main.c: see ticks_* in shmem.h */
#define PS_TICKS_REM   32
#define PS_TICKS_DEN   36
#define PS_MIN_CVR     40  /* PROFILE: lowest SysTick count seen at end of work */
#define PS_OVERRUNS    44  /* PROFILE: ticks that ran into the next one */
#define PS_SIZE        48

#ifndef __ASSEMBLER__
#include <stdint.h>

extern uint32_t m0_play_state[PS_SIZE / 4];

/* SysTick running at the tick rate, PRIMASK set, shmem header valid.
 * Returns when shmem->ctrl leaves M0_CTRL_PLAY, pins as the last tick left them. */
void m0_play(void);
#endif

#endif /* M0_PLAY_H */
