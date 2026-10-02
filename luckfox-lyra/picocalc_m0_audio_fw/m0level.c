// SPDX-License-Identifier: GPL-2.0
/*
 * m0level SECONDS [BIN_MS]: read the levels of the M0 audio pins (GPIO4_B2
 * left, GPIO4_B3 right) through /dev/mem as fast as possible and print the
 * fraction of reads that found each high: the output level the firmware is
 * actually producing, as a duty cycle (0.5 = zero, 0.5 + x * 7/8 / 65536 for
 * a 16-bit sample x). With BIN_MS, one line per BIN_MS milliseconds, to see
 * a slow tone. With FREQ (m0level SECONDS 0 FREQ) also the amplitude of a tone
 * of that frequency on the left pin, by correlating the reads with it: the
 * M0's sample clock and this clock come from the same crystal, so the
 * frequency is exact; above a kilohertz or so it reads low, the reads being
 * timed in blocks. Reads do not disturb the M0. Run as root.
 */
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>

#define GPIO4_BASE	0xFF1E0000UL
#define GPIO_EXT_PORT	0x70

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
	double secs = argc > 1 ? atof(argv[1]) : 2, bin = argc > 2 ? atof(argv[2]) / 1000 : 0;
	double t0, t, tb, lmin = 1, lmax = 0, freq = argc > 3 ? atof(argv[3]) : 0, tp, sc = 0, ss = 0;
	static unsigned char buf[256];
	unsigned long n = 0, l = 0, r = 0, bn = 0, bl = 0, br = 0;
	volatile uint32_t *p;
	void *map;
	int fd = open("/dev/mem", O_RDONLY | O_SYNC), i;

	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	map = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, GPIO4_BASE);
	if (map == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	p = (volatile uint32_t *)((char *)map + GPIO_EXT_PORT);
	t0 = tb = tp = now();
	do {
		for (i = 0; i < 256; i++) {
			uint32_t v = *p;

			buf[i] = (v >> 10) & 1;
			bl += (v >> 10) & 1;
			br += (v >> 11) & 1;
		}
		bn += 256;
		t = now();
		if (freq) {	/* the reads of this block are evenly spread over tp..t */
			double w = 2 * M_PI * freq, ph = w * (tp - t0), dph = w * (t - tp) / 256;

			for (i = 0; i < 256; i++, ph += dph) {
				sc += (buf[i] - 0.5) * cos(ph);
				ss += (buf[i] - 0.5) * sin(ph);
			}
		}
		tp = t;
		if (bin && t - tb >= bin) {
			double d = (double)bl / bn;

			printf("%7.3f  L %.4f  R %.4f\n", t - t0, d, (double)br / bn);
			if (d < lmin)
				lmin = d;
			if (d > lmax)
				lmax = d;
			n += bn; l += bl; r += br;
			bn = bl = br = 0;
			tb = t;
		}
	} while (t - t0 < secs);
	n += bn; l += bl; r += br;
	printf("%lu reads in %.2f s: left high %.4f, right high %.4f", n, t - t0,
	       (double)l / n, (double)r / n);
	if (freq)
		printf("; left amplitude at %.0f Hz %.4f", freq, 2 * sqrt(sc * sc + ss * ss) / n);
	if (bin)
		printf("; left swings %.4f..%.4f (amplitude %.4f)", lmin, lmax, (lmax - lmin) / 2);
	printf("\n");
	return 0;
}
