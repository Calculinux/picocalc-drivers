/* SPDX-License-Identifier: GPL-2.0
 * Host-side, bit-exact mirror of the delta-sigma arithmetic in play.S.
 * cc -O2 -Wall -Wextra -o check_dsm check_dsm.c -lm && ./check_dsm
 *
 * Checks, for one channel, that over the whole 16-bit input range
 *   - the output duty tracks the (7/8-scaled) input with unity gain,
 *   - ordinary programme (DC, sines) never reaches the integrator clamp, and
 *   - hostile input (full-scale noise, rail-to-rail at Nyquist) stays far
 *     from int32 wrap, with the clamp applied once per sample as play.S does.
 * Any change to the modulator or the prefetch scaling in play.S must be made
 * here too. check_play.py (make check-emu) runs the real machine code.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "rk3506_regs.h"
#include "shmem.h"

#define TICK_HZ     1000000U	/* any rate the firmware might run at */
#define TICKS       2000000U	/* 2 s per stimulus */
#define CLAMP       (1 << DSM_CLAMP_SHIFT)
#define STATE_LIMIT (1 << 26)	/* between clamps the state may overshoot; 2^24 is the most seen */

struct dsm {
	int32_t i1, i2, g, x;
	unsigned long clamped;
};

/* play.S step 6/7: x -= x >> 3 (arithmetic shift) */
static int32_t scale_in(int16_t x)
{
	return (int32_t)x - ((int32_t)x >> 3);
}

/* play.S TICK, one channel. i1 carries the previous -G, as in the firmware.
 * Unsigned arithmetic wraps like the registers do. */
static uint32_t dsm_tick(struct dsm *d)
{
	uint32_t i1 = (uint32_t)d->i1 + (uint32_t)d->x;
	uint32_t y = (uint32_t)d->i2 + i1;
	int32_t s = (int32_t)y >> 31;			/* 0: bit 1, -1: bit 0 */
	uint32_t g = ((uint32_t)s << 16) + (1U << DSM_FS_SHIFT);

	d->i2 = (int32_t)(y - g);
	d->i1 = (int32_t)(i1 - g);
	return (uint32_t)(s + 1);
}

/* play.S CLAMP, once per sample */
static void dsm_clamp(struct dsm *d)
{
	if ((((uint32_t)((d->i2 >> DSM_CLAMP_SHIFT) + 1)) >> 1) != 0) {
		d->i2 = (d->i2 >> 31) ^ (CLAMP - 1);
		d->clamped++;
	}
}

struct result {
	double duty;
	int64_t peak1, peak2;
	unsigned long clamped;
};

static uint32_t rng = 12345;

static int32_t rnd16(void)
{
	rng = rng * 1664525u + 1013904223u;
	return (int32_t)(rng >> 16) - 32768;
}

static struct result run(int16_t (*gen)(uint32_t n, int32_t arg), int32_t arg)
{
	struct dsm d = { -(1 << DSM_FS_SHIFT), 0, 0, 0, 0 };
	struct result r = { 0, 0, 0, 0 };
	uint32_t phase = 0, n = 0, t;
	uint64_t ones = 0;

	for (t = 0; t < TICKS; t++) {
		phase += M0_SAMPLE_RATE_HZ;
		if (phase >= TICK_HZ) {
			phase -= TICK_HZ;
			d.x = scale_in(gen(n++, arg));
			dsm_clamp(&d);
		}
		ones += dsm_tick(&d);
		if (llabs((long long)d.i1) > r.peak1)
			r.peak1 = llabs((long long)d.i1);
		if (llabs((long long)d.i2) > r.peak2)
			r.peak2 = llabs((long long)d.i2);
	}
	r.duty = (double)ones / TICKS;
	r.clamped = d.clamped;
	return r;
}

static int16_t gen_dc(uint32_t n, int32_t arg)
{
	(void)n;
	return (int16_t)arg;
}

static int16_t gen_sine_440(uint32_t n, int32_t amp)
{
	return (int16_t)lrint(amp * sin(2.0 * M_PI * 440.0 * n / M0_SAMPLE_RATE_HZ));
}

static int16_t gen_square_1k(uint32_t n, int32_t amp)
{
	return (n / 24) & 1 ? (int16_t)-amp : (int16_t)amp;
}

static int16_t gen_noise(uint32_t n, int32_t arg)
{
	(void)n;
	(void)arg;
	return (int16_t)rnd16();
}

static int16_t gen_rails(uint32_t n, int32_t arg)
{
	(void)n;
	(void)arg;
	return rnd16() > 0 ? 32767 : -32768;
}

static int fails;

/* clean: the stimulus must stay clear of the clamp altogether */
static void check(const char *name, int32_t arg, struct result r, double want,
		  double tol, int clean)
{
	int bad = fabs(r.duty - want) > tol ||
		  r.peak2 > STATE_LIMIT || r.peak1 > STATE_LIMIT ||
		  (clean && r.clamped);

	printf("%-12s %6d  duty %.4f (want %.4f)  peak i1 %7lld  i2 %7lld  clamped %7lu  %s\n",
	       name, arg, r.duty, want, (long long)r.peak1, (long long)r.peak2,
	       r.clamped, bad ? "FAIL" : "ok");
	fails += bad;
}

int main(void)
{
	static const int32_t dc[] = { -32768, -28000, -16384, -8000, -1, 0, 1,
				      8000, 16384, 20000, 28000, 32767 };
	static const int32_t amp[] = { 100, 4000, 20000, 32767 };
	size_t i;

	for (i = 0; i < sizeof(dc) / sizeof(dc[0]); i++)
		check("dc", dc[i], run(gen_dc, dc[i]),
		      0.5 + scale_in((int16_t)dc[i]) / 65536.0, 0.001, 1);
	for (i = 0; i < sizeof(amp) / sizeof(amp[0]); i++)
		check("sine 440", amp[i], run(gen_sine_440, amp[i]), 0.5, 0.001, 1);
	/* A full-scale square may brush the clamp; it must still average out. */
	for (i = 0; i < sizeof(amp) / sizeof(amp[0]); i++)
		check("square 1k", amp[i], run(gen_square_1k, amp[i]), 0.5, 0.001,
		      amp[i] < 32767);
	/* These wrap int32 without the clamp. */
	check("noise", 0, run(gen_noise, 0), 0.5, 0.01, 0);
	check("rails", 0, run(gen_rails, 0), 0.5, 0.01, 0);

	if (fails) {
		printf("check_dsm: %d FAILED\n", fails);
		return 1;
	}
	puts("check_dsm: ok");
	return 0;
}
