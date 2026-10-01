/* SPDX-License-Identifier: GPL-2.0
 * Host-side check of the M0 DSM (delta-sigma modulator) maths (no kernel).
 * Bit-exact replica of the asm operations in isr.S (modular int32).
 *
 * make check_dsm && ./check_dsm [stimulus 1..5]
 *   1 DC +FS (default; the canonical B3 failure)
 *   2 +-FS square   3 440 Hz sine @FS   4 DC +8000
 *   5 two-tone 12000 (440+880 Hz)
 *
 * Spec under test (see ../../docs/m0-audio-review.md, finding B3):
 *   - duty of the 1-bit output tracks (x_avg + HALF) / FULL
 *   - neither integrator ever approaches 2^30 (int32 wrap == corrupt audio)
 * Exit status: 0 = spec met, 1 = violation. With the current isr.S maths,
 * stimulus 1 FAILS by design (stability boundary crossed, integrators wrap);
 * re-run after any DSM change.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define DSM_TICKS 1000000UL /* 1 s at 1 MHz */
#define LIMIT     (1L << 30)

static const int32_t HALF = 16384;
static const int32_t FULL = 32768;

static void dsm_step(int32_t *i1, int32_t *i2, int32_t last, int32_t *out)
{
	/* Note: parentheses matter — in C, '<<' binds LOOSER than '+/-' and
	 * an unparenthesised port silently shifts the whole accumulation.
	 * Mirrors: lsls fb, out, #15; subs i1, i1, fb
	 */
	*i1 = *i1 + last + HALF - ((*out) << 15);
	/* y = integ2 + integ1_new; out = sign(y) in {0,1} (asrs #31 + 1,
	 * i.e. y == 0 -> 1, matching the asm's zero-sign convention) */
	int32_t y = *i2 + *i1;
	int32_t no = (y >> 31) + 1;
	/* integ2 += integ1_new + FULL - (out_new << 16) */
	*i2 = y + FULL - (no << 16);
	*out = no;
}

int main(int argc, char **argv)
{
	int mode = (argc > 1) ? atoi(argv[1]) : 1;
	int32_t i1 = 0, i2 = 0, out = 0, last = 0;
	uint64_t ones = 0, ticks = 0;
	uint32_t phase = 0;
	int32_t mn1 = 0, mx1 = 0, mn2 = 0, mx2 = 0;

	for (uint64_t tk = 0; tk < DSM_TICKS; tk++) {
		phase += 48000;
		if (phase >= 1000000) { /* consume one sample (48 kHz avg) */
			phase -= 1000000;
			double t = (double)(tk * 48000) / 1e6;
			if (mode == 1)
				last = 32767;
			else if (mode == 2)
				last = ((tk / 100) % 2) ? 32767 : -32767;
			else if (mode == 3)
				last = (int32_t)(32767 * sin(2 * M_PI * 440 * t));
			else if (mode == 4)
				last = 8000;
			else
				last = (int32_t)(12000 * sin(2 * M_PI * 440 * t) +
					     12000 * sin(2 * M_PI * 880 * t));
		}
		dsm_step(&i1, &i2, last, &out);
		if (i1 < mn1) mn1 = i1;
		if (i1 > mx1) mx1 = i1;
		if (i2 < mn2) mn2 = i2;
		if (i2 > mx2) mx2 = i2;
		ones += out;
		ticks++;
	}

	double duty = (double)ones / ticks;
	int fail = 0;
	printf("mode %d: duty=%.3f  integ1[%d .. %d]  integ2[%d .. %d]\n",
	       mode, duty, mn1, mx1, mn2, mx2);
	if (mn1 < -LIMIT || mx1 > LIMIT) {
		printf("FAIL: integ1 exceeded +/-2^30 (wrap = corrupt audio)\n");
		fail = 1;
	}
	if (mn2 < -LIMIT || mx2 > LIMIT) {
		printf("FAIL: integ2 exceeded +/-2^30 (wrap = corrupt audio)\n");
		fail = 1;
	}
	/* duty-tracking spot check for the one stimulus inside the spec
	 * envelope (levels above half-scale are the failure case itself) */
	if (mode == 4) {
		double want = (8000.0 + HALF) / FULL;
		if (fabs(duty - want) > 0.01) {
			printf("FAIL: duty %.3f != expected %.3f\n", duty, want);
			fail = 1;
		}
	}
	return fail;
}
