// SPDX-License-Identifier: GPL-2.0
/*
 * m0hammer ADDR SECONDS [w] [GAP_US]: read (or, with w, write zero to) one
 * 32-bit word through /dev/mem as fast as possible, or once every GAP_US
 * microseconds, to see what host accesses to a bus slave cost the M0 while
 * it plays (docs/m0-audio.md, "What the ticking was"). Run as root. Take
 * care what you write to: 0xFFF83F00 is a spare word of the SRAM.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
	return ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(int argc, char **argv)
{
	unsigned long addr, n = 0, gap = 0;
	volatile uint32_t *p;
	double secs, t0, t;
	uint32_t v = 0;
	int fd, wr;
	void *map;

	if (argc < 3) {
		fprintf(stderr, "usage: m0hammer ADDR SECONDS [w] [GAP_US]\n");
		return 1;
	}
	addr = strtoul(argv[1], NULL, 0);
	secs = atof(argv[2]);
	wr = argc > 3 && argv[3][0] == 'w';
	if (argc > 4)
		gap = strtoul(argv[4], NULL, 0);

	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	map = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED, fd, addr & ~0xFFFUL);
	if (map == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	p = (volatile uint32_t *)((char *)map + (addr & 0xFFFUL));

	t0 = now();
	do {
		int i;

		for (i = 0; i < 64; i++) {
			if (wr)
				*p = v;
			else
				v = *p;
			n++;
			if (gap) {
				usleep(gap);
				break;
			}
		}
		t = now();
	} while (t - t0 < secs);
	printf("m0hammer: %lu %s of %#lx in %.2f s (%.0f/s, %.0f ns each)\n", n,
	       wr ? "writes" : "reads", addr, t - t0, n / (t - t0), (t - t0) / n * 1e9);
	return 0;
}
