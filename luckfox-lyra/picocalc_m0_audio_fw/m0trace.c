// SPDX-License-Identifier: GPL-2.0
/*
 * m0trace: read the event log a PROFILE=1 build of the M0 audio firmware
 * keeps in system SRAM (shmem.h, M0_TRACE_ADDR), through /dev/mem. Run as
 * root while a stream plays: its end, when the ring runs dry, fills the log
 * with that. The log holds the last 256 events:
 * ticks that were still working when the next one fell due (with the step
 * of the sample they were in) and samples for which the ring was empty.
 *
 *   m0trace              summary, timeline and gaps
 *   m0trace -p FRAMES    also: where in the ALSA period (FRAMES long) the
 *                        events fall, to tie them to the host's ring updates
 *   m0trace -v           list every event
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define M0_SHMEM_ADDR     0xFFF81000U
#define M0_TRACE_ADDR     (M0_SHMEM_ADDR + 0x2100U)
#define M0_TRACE_ENTRIES  256
#define M0_TRACE_UNDERRUN 30
#define M0_TRACE_PLAIN    31
#define SAMPLE_RATE       48000

static const char *const step_name[] = {
	[1] = "stop check (bus)", [2] = "read write_idx (bus)", [3] = "next frame",
	[4] = "fetch frame (bus)", [5] = "compute read_idx", [6] = "publish read_idx (bus)",
	[7] = "publish longest tick (bus)", [8] = "publish overruns (bus)",
	[9] = "count sample", [10] = "sample length", [11] = "left in",
	[12] = "left start", [13] = "right in", [14] = "right start",
	[15] = "left go", [16] = "right go", [17] = "clamp left", [18] = "clamp right",
	[M0_TRACE_UNDERRUN] = "RING EMPTY", [M0_TRACE_PLAIN] = "plain tick",
};

int main(int argc, char **argv)
{
	uint32_t ev[M0_TRACE_ENTRIES], total, n, i, period = 0, by_code[32] = { 0 };
	uint32_t first, last, span;
	volatile uint32_t *t;
	int verbose = 0, fd, a;
	void *map;

	for (a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "-v"))
			verbose = 1;
		else if (!strcmp(argv[a], "-p") && a + 1 < argc)
			period = strtoul(argv[++a], NULL, 0);
	}

	fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0) {
		perror("/dev/mem");
		return 1;
	}
	map = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, fd, M0_TRACE_ADDR & ~0xFFFU);
	if (map == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	t = (volatile uint32_t *)((char *)map + (M0_TRACE_ADDR & 0xFFFU));

	total = t[0];
	n = total < M0_TRACE_ENTRIES ? total : M0_TRACE_ENTRIES;
	for (i = 0; i < n; i++)		/* oldest first */
		ev[i] = t[1 + ((total - n + 1 + i) & (M0_TRACE_ENTRIES - 1))];

	printf("%u events since the stream started", total);
	if (!n) {
		printf(".\n(Needs a PROFILE=1 firmware; a normal build logs nothing.)\n");
		return 0;
	}
	first = ev[0] >> 5;
	last = ev[n - 1] >> 5;
	span = last - first;
	printf("; the last %u cover samples %u..%u (%.2f s, %.1f events/s)\n",
	       n, first, last, (double)span / SAMPLE_RATE,
	       span ? (double)(n - 1) * SAMPLE_RATE / span : 0.0);

	for (i = 0; i < n; i++)
		by_code[ev[i] & 31]++;
	printf("\nBy kind:\n");
	for (i = 0; i < 32; i++)
		if (by_code[i])
			printf("  %5u  %-2u %s\n", by_code[i], i, step_name[i] ? step_name[i] : "?");
	if (by_code[M0_TRACE_UNDERRUN])
		printf("  (a few RING EMPTY events at the very end of a stream are normal:\n"
		       "   the data runs out before the stop arrives)\n");

	/* When: one character per 50 ms */
	if (span) {
		uint32_t bins = span / (SAMPLE_RATE / 20) + 1, b, k = 0;

		if (bins > 400)
			bins = 400;
		printf("\nTimeline, 50 ms per character (. none, 1-9 events, # ten or more, E ring empty):\n  ");
		for (b = 0; b < bins; b++) {
			uint32_t lo = first + b * (SAMPLE_RATE / 20), hi = lo + SAMPLE_RATE / 20, c = 0;
			int empty = 0;

			while (k < n && (ev[k] >> 5) < hi) {
				c++;
				if ((ev[k] & 31) == M0_TRACE_UNDERRUN)
					empty = 1;
				k++;
			}
			putchar(empty ? 'E' : !c ? '.' : c < 10 ? (char)('0' + c) : '#');
			if (b % 80 == 79)
				printf("\n  ");
		}
		printf("\n");
	}

	/* Gaps between consecutive events */
	if (n > 1) {
		static const uint32_t edge[] = { 1, 5, 48, 480, 2400, 12000, 24000, 48000 };
		static const char *const label[] = {
			"same sample", "< 0.1 ms", "< 1 ms", "< 10 ms", "< 50 ms",
			"< 250 ms", "< 500 ms", "< 1 s", ">= 1 s",
		};
		uint32_t g[9] = { 0 }, j;

		for (i = 1; i < n; i++) {
			uint32_t d = (ev[i] >> 5) - (ev[i - 1] >> 5);

			for (j = 0; j < 8 && d >= edge[j]; j++)
				;
			g[j]++;
		}
		printf("\nGap to the previous event:\n");
		for (j = 0; j < 9; j++)
			if (g[j])
				printf("  %5u  %s\n", g[j], label[j]);
	}

	/* Position within the host's period: flat = unrelated, peaked = tied to it */
	if (period) {
		uint32_t h[16] = { 0 };

		for (i = 0; i < n; i++)
			h[(uint64_t)((ev[i] >> 5) % period) * 16 / period]++;
		printf("\nPosition within a %u-frame period, in sixteenths:\n ", period);
		for (i = 0; i < 16; i++)
			printf(" %u", h[i]);
		printf("\n");
	}

	if (verbose) {
		printf("\n");
		for (i = 0; i < n; i++)
			printf("  sample %9u (%8.3f s)  %s\n", ev[i] >> 5,
			       (double)(ev[i] >> 5) / SAMPLE_RATE,
			       step_name[ev[i] & 31] ? step_name[ev[i] & 31] : "?");
	}
	return 0;
}
