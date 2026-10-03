/* SPDX-License-Identifier: GPL-2.0
 * Host-side check of M0 ring copy/space/underrun math (no kernel).
 * cc -O2 -Wall -Wextra -o check_ring check_ring.c && ./check_ring
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#define BUF 8192U
#define MASK (BUF - 1U)
#define FRAME 4U
#define PERIOD 1024U
#define ALSA_BUF 32768U
#define BOUNDARY (ALSA_BUF * 2U)

static uint32_t ring_space(uint32_t write_idx, uint32_t read_idx, uint32_t buf_size)
{
	if (write_idx >= read_idx)
		return buf_size - (write_idx - read_idx) - 1;
	return read_idx - write_idx - 1;
}

/* appl and copied live in the same wrap domain as ALSA boundary, not buffer offset. */
static uint32_t appl_avail(uint32_t appl, uint32_t copied, uint32_t boundary)
{
	if (appl >= copied)
		return appl - copied;
	return boundary - copied + appl;
}

/* As much as the ring takes: not capped at one period (see the driver). */
static uint32_t copy_size(uint32_t space, uint32_t avail, uint32_t frame)
{
	uint32_t n = space;

	if (n > avail)
		n = avail;
	return n - (n % frame);
}

static int consume(uint32_t *read_idx, uint32_t write_idx, uint32_t mask)
{
	if (write_idx == *read_idx)
		return 0;
	*read_idx = (*read_idx + FRAME) & mask;
	return 1;
}

int main(void)
{
	uint32_t w = 0, r = 0, copied = 0, appl = 0, n, i;

	assert(ring_space(0, 0, BUF) == BUF - 1);
	assert(ring_space(0, 4, BUF) == 3);
	assert(ring_space(100, 0, BUF) == BUF - 100 - 1);

	/* Space-limited copy must stay frame-aligned (old bug: space = 4k-1). */
	n = copy_size(ring_space(0, 0, BUF), PERIOD, FRAME);
	assert(n == PERIOD);
	n = copy_size(ring_space(0, 0, BUF), 8192, FRAME);
	assert(n == 8192 - 4);
	assert(n % FRAME == 0);
	/* A full PCM buffer at START fills the ring, not just one period. */
	n = copy_size(ring_space(0, 0, BUF), ALSA_BUF, FRAME);
	assert(n == BUF - FRAME);

	/* Empty vs full PCM buffer at START (old bug: both looked like avail=0). */
	assert(appl_avail(0, 0, BOUNDARY) == 0);
	assert(appl_avail(ALSA_BUF, 0, BOUNDARY) == ALSA_BUF);
	assert(appl_avail(PERIOD, 0, BOUNDARY) == PERIOD);

	/* Streaming past one ALSA buffer; appl/copied wrap at boundary. */
	for (i = 0; i < 40; i++) {
		appl += PERIOD;
		if (appl >= BOUNDARY)
			appl -= BOUNDARY;
		n = copy_size(ring_space(w, r, BUF),
			      appl_avail(appl, copied, BOUNDARY), FRAME);
		assert(n > 0);
		w = (w + n) & MASK;
		copied += n;
		if (copied >= BOUNDARY)
			copied -= BOUNDARY;
		{
			uint32_t left = n;

			while (left >= FRAME) {
				assert(consume(&r, w, MASK) == 1);
				left -= FRAME;
			}
		}
	}

	/* Underrun: hold last, do not advance read_idx. */
	r = w;
	assert(consume(&r, w, MASK) == 0);
	assert(r == w);
	w = (w + FRAME) & MASK;
	assert(consume(&r, w, MASK) == 1);
	assert(r == w);

	/* Wrap write past end of ring. */
	w = BUF - FRAME;
	r = 0;
	n = copy_size(ring_space(w, r, BUF), PERIOD, FRAME);
	assert(n % FRAME == 0);
	w = (w + n) & MASK;
	assert(w < BUF);

	/* Boundary wrap: appl behind copied in unsigned compare. */
	assert(appl_avail(PERIOD, BOUNDARY - PERIOD, BOUNDARY) == 2 * PERIOD);

	puts("check_ring: ok");
	return 0;
}
