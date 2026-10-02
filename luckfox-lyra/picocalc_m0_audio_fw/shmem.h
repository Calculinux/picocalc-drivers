/* SPDX-License-Identifier: GPL-2.0 */
/* Shared memory layout between Linux driver and M0 (must match both sides) */

#ifndef M0_SHMEM_H
#define M0_SHMEM_H

#ifdef __ASSEMBLER__
#define M0_U32(x)         x
#else
#include <stdint.h>
#define M0_U32(x)         x##U
#endif

#define M0_AUDIO_MAGIC    0x4D304431U  /* "M0D1" M0 audio */
#define M0_CTRL_PLAY      1
#define M0_CTRL_STOP      0

/*
 * The firmware is loaded once and left running. Between streams it idles:
 * SysTick slowed to one wake every M0_IDLE_CYCLES core cycles (a few ms),
 * asleep in WFI in between, looking at ctrl on each wake.
 *
 *   host: wait for m0_state == IDLE, fill in the header, zero the ring
 *         frame just before read_idx (it is what plays until the first
 *         frame is fetched, and on an underrun straight away), ctrl = PLAY
 *   M0:   sees PLAY at its next idle wake, m0_state = PLAY, plays
 *   host: ctrl = STOP
 *   M0:   sees STOP within one sample, drives the pins low, m0_state = IDLE
 *
 * The host must not touch the indices or the timing while m0_state is PLAY.
 */
#define M0_STATE_IDLE     0x49444C45U  /* "IDLE" */
#define M0_STATE_PLAY     0x504C4159U  /* "PLAY" */
#define M0_IDLE_CYCLES    (1U << 19)   /* 2.8 ms at 187.5 MHz */
#define M0_FMT_U8         0
#define M0_FMT_S16_LE     1

/*
 * The header and ring live in system SRAM, so the M0 never touches DDR while
 * playing (M0 accesses to DDR are very slow). They sit in the first bank,
 * 0xFFF80000..0xFFF84000, past the 4 KB OP-TEE keeps for itself, because that
 * bank stays on the ordinary bus whichever way the firmware is run:
 *   bus mode   image at 0xFFF88000 (third bank), reloadable
 *   TCM mode   image at 0xFFF84000; the second and third banks become the
 *              M0's private memory and Linux can no longer reach them
 * Either way the M0 sees its image at address 0 (rk3506-m0-audio.ld).
 *   0xFFF81000  this header (64 B) + 8192 B ring
 * Both sides use the absolute address: the M0 as a plain data access, the
 * host through the node its memory-region property points at.
 * (At boot this area holds the remains of the DDR-init stage; it is free.)
 */
#define M0_SHMEM_ADDR     M0_U32(0xFFF81000)

/* Fixed stream format: S16_LE stereo. Host and firmware must agree. */
#define M0_SAMPLE_RATE_HZ M0_U32(48000)
#define M0_RING_BYTES     M0_U32(8192)
#define M0_FRAME_BYTES    4

/* Header field offsets for play.S (checked against the struct in main.c) */
#define M0_SHMEM_CTRL          4
#define M0_SHMEM_WRITE_IDX     8
#define M0_SHMEM_READ_IDX      12
#define M0_SHMEM_STAT_MIN_CVR  56
#define M0_SHMEM_STAT_OVERRUNS 60

/* One sample must last at least this many ticks: play.S gives each step of
 * the per-sample work a tick of its own (18 in a PROFILE build) and needs one
 * more. */
#define M0_MIN_TICKS_PER_SAMPLE 19

/* flags */
#define M0_FLAG_NO_INTERP 1   /* hold each sample; default is to ramp to the next */

/*
 * Event log of a PROFILE=1 firmware, in the SRAM after the ring; read it with
 * m0trace. Word 0 counts the events since the stream started; event n is in
 * word 1 + (n & (M0_TRACE_ENTRIES - 1)), as (sample number << 5) | code:
 *   1..18  that step tick was still working when the next tick fell due
 *   31     a plain tick was
 *   30     the ring was empty when the firmware wanted the next frame
 */
#define M0_TRACE_ADDR     (M0_SHMEM_ADDR + M0_U32(0x2100))
#define M0_TRACE_ENTRIES  256
#define M0_TRACE_UNDERRUN 30
#define M0_TRACE_PLAIN    31

#ifndef __ASSEMBLER__
typedef struct {
	volatile uint32_t magic;
	volatile uint32_t ctrl;
	volatile uint32_t write_idx;
	volatile uint32_t read_idx;
	volatile uint32_t m0_state;   /* written by the M0: M0_STATE_* */
	volatile uint32_t buf_size;
	volatile uint32_t sample_rate;
	volatile uint32_t channels;
	volatile uint32_t format;
	volatile uint32_t flags;   /* M0_FLAG_* */
	/*
	 * Tick timing, set by the host, which knows the M0 core clock (hclk_m0):
	 *   tick_cycles  core clock cycles per tick (one output bit per tick)
	 *   ticks_base   whole ticks per sample = hclk / (tick_cycles * sample_rate)
	 *   ticks_frac   the fractional part of that, in units of 2^-32 tick
	 * A sample lasts ticks_base ticks, plus one whenever ticks_frac, added up
	 * once per sample, overflows 32 bits. All zero: firmware defaults.
	 */
	volatile uint32_t tick_cycles;
	volatile uint32_t ticks_base;
	volatile uint32_t ticks_frac;
	volatile uint32_t _reserved;
	/*
	 * Written by a PROFILE=1 firmware build, read by the host: the lowest
	 * SysTick count seen at the end of a tick's work (the tick's work took
	 * tick_cycles - 1 - stat_min_cvr cycles), and how many ticks were still
	 * working when the next one fell due. 0xFFFFFFFF / 0 = no data.
	 */
	volatile uint32_t stat_min_cvr;
	volatile uint32_t stat_overruns;
	uint8_t           buffer[];
} m0_audio_shmem_t;
#endif

#define M0_HEADER_SIZE    64

#endif /* M0_SHMEM_H */
