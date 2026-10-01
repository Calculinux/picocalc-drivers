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
#define M0_FMT_U8         0
#define M0_FMT_S16_LE     1

/*
 * The header and ring live in system SRAM, in the same 16 KB bank as the
 * firmware, so the M0 never touches DDR while playing:
 *   0xFFF88000  firmware image + stack (4 KB, seen by the M0 at address 0,
 *               see rk3506-m0-audio.ld)
 *   0xFFF89000  this header (64 B) + 8192 B ring
 * Both sides use the absolute address: the M0 as a plain data access, the
 * host through the m0_shmem reserved-memory node (mapped WC).
 */
#define M0_SHMEM_ADDR     M0_U32(0xFFF89000)

/* Fixed stream format: S16_LE stereo. Host and firmware must agree. */
#define M0_SAMPLE_RATE_HZ M0_U32(48000)
#define M0_RING_BYTES     M0_U32(8192)
#define M0_FRAME_BYTES    4

/* Header field offsets for play.S (checked against the struct in main.c) */
#define M0_SHMEM_CTRL      4
#define M0_SHMEM_WRITE_IDX 8
#define M0_SHMEM_READ_IDX  12

#ifndef __ASSEMBLER__
typedef struct {
	volatile uint32_t magic;
	volatile uint32_t ctrl;
	volatile uint32_t write_idx;
	volatile uint32_t read_idx;
	volatile uint32_t period_bytes;
	volatile uint32_t buf_size;
	volatile uint32_t sample_rate;
	volatile uint32_t channels;
	volatile uint32_t format;
	volatile uint32_t flags;   /* M0_SHMEM_FLAG_* */
	uint32_t          _reserved[6];
	uint8_t           buffer[];
} m0_audio_shmem_t;
#endif

#define M0_HEADER_SIZE    64

/* flags: set by host when it supports wake via GRF rxev / WIC */
#define M0_SHMEM_FLAG_WIC_WAKE  (1u << 0)  /* Host set wicenreq and will wake via rxev; M0 may use WFI+SLEEPDEEP */

#endif /* M0_SHMEM_H */
