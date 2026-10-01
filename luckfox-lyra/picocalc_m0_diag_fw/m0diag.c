// SPDX-License-Identifier: GPL-2.0
/*
 * m0diag: read and interpret the results of the RK3506 M0 diagnostic firmware
 * (rk3506-m0-diag.elf) from Linux, through /dev/mem. Run as root.
 *
 *   m0diag            print everything, then sample the heartbeat for 2 s
 *   m0diag -r         print the SoC registers only (works with any firmware,
 *                     or none: M0 sleep/lockup state and isolation bits)
 */
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "diag.h"

#define GRF_BASE         0xFF288000U
#define GRF_SOC_STATUS2  0x0108U
#define GRF_PMU_BASE     0xFF911000U  /* second 4 KB of GRF_PMU: the ISO registers */
#define TIMER_HZ         100000000.0  /* nominal clk_gpll_div_100m */

static int memfd;
static sigjmp_buf bus_fault;

static void on_sigbus(int sig)
{
	(void)sig;
	siglongjmp(bus_fault, 1);
}

static volatile uint32_t *map(uint32_t phys)
{
	void *p = mmap(NULL, 0x1000, PROT_READ, MAP_SHARED, memfd, phys & ~0xFFFU);

	if (p == MAP_FAILED) {
		fprintf(stderr, "mmap 0x%08x: ", phys);
		perror(NULL);
		exit(1);
	}
	return (volatile uint32_t *)((char *)p + (phys & 0xFFFU));
}

static double now(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static const char *stage_name(uint32_t s)
{
	static const char *const names[] = {
		"(none)", "ALIVE", "CAL", "PERIOD", "POLL_LAT", "POLL_SLOW",
		"WFI_SLOW", "WFI_LAT", "RUN",
	};

	return s < sizeof(names) / sizeof(names[0]) ? names[s] : "(unknown)";
}

static void soc_registers(void)
{
	volatile uint32_t *grf = map(GRF_BASE);
	volatile uint32_t *iso = map(GRF_PMU_BASE);
	unsigned int i, sleeping = 0, gate = 0, lockup = 0, any = 0;
	const unsigned int samples = 20000;
	uint32_t v;

	for (i = 0; i < samples; i++) {
		v = grf[GRF_SOC_STATUS2 / 4];
		sleeping += (v >> 6) & 1;
		gate += (v >> 4) & 1;
		lockup |= (v >> 3) & 1;
	}
	printf("GRF_SOC_STATUS2 (0xFF288108), %u samples:\n", samples);
	printf("  M0 sleeping      %5.1f %% of samples\n", 100.0 * sleeping / samples);
	printf("  hclk can be gated %4.1f %% of samples\n", 100.0 * gate / samples);
	printf("  M0 locked up     %s\n", lockup ? "YES (double fault)" : "no");

	printf("GRF_PMU_MCU_ISO_CON0..11 (0xFF911000; a set bit blocks the M0 from that peripheral;\n"
	       "  reset value is 0 except CON8 = 0x00000100):\n ");
	/* Linux itself may be isolated from this block; do not die on the read. */
	signal(SIGBUS, on_sigbus);
	if (sigsetjmp(bus_fault, 1)) {
		printf("\n  not readable from Linux (bus error)\n");
		signal(SIGBUS, SIG_DFL);
		return;
	}
	for (i = 0; i < 12; i++) {
		v = iso[i];
		printf(" %08x", v);
		if ((i == 8 ? v & ~0x100U : v) & 0xFFFFU)
			any = 1;
	}
	printf("\n  MCU_ISO_DDR_CON0/1 %08x %08x  LOCK %08x\n",
	       iso[0x400 / 4], iso[0x404 / 4], iso[0x500 / 4]);
	printf("  %s\n", any ? "NOT at reset values: check which peripherals are blocked"
			     : "at reset values: nothing extra is blocked");
	signal(SIGBUS, SIG_DFL);
}

static void latency(const char *name, const volatile struct m0_diag_lat *l, double mhz)
{
	uint32_t n = l->n;

	if (!n) {
		printf("%s: no ticks captured (spurious WFI returns: %u)\n", name, l->spurious);
		return;
	}
	if (n < M0_DIAG_LAT_N) {
		printf("%s: in progress or stuck at %u of %u ticks (spurious %u)\n",
		       name, n, M0_DIAG_LAT_N, l->spurious);
		return;
	}
	printf("%s: expiry-to-detect over %u ticks, in 10 ns timer counts:\n", name, n);
	printf("  min %u  max %u  mean %.2f  spread %u counts = %.0f ns (about %.0f CPU cycles)\n",
	       l->min, l->max, (double)l->sum / n, l->max - l->min,
	       (l->max - l->min) * 10.0, (l->max - l->min) * mhz / 100.0);
}

int main(int argc, char **argv)
{
	const volatile struct m0_diag *d;
	double mhz, cyc_per_count, t0, t1;
	uint32_t stage, h0, h1;

	memfd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (memfd < 0) {
		perror("/dev/mem");
		return 1;
	}

	soc_registers();
	if (argc > 1 && !strcmp(argv[1], "-r"))
		return 0;

	d = (const volatile struct m0_diag *)map(M0_DIAG_ADDR);
	printf("\nResults block at 0x%08x:\n", M0_DIAG_ADDR);
	if (d->magic != M0_DIAG_MAGIC) {
		printf("  no diagnostic results (magic %08x): the diagnostic firmware has not\n"
		       "  run, did not get as far as main(), or cannot write this SRAM.\n",
		       d->magic);
		return 2;
	}
	stage = d->stage;
	printf("  version %u, stage %u %s%s\n", d->version, stage, stage_name(stage),
	       d->fault ? "  ** HardFault during this stage **" : "");
	if (stage < M0_DIAG_PERIOD && !d->cal_sram_low)
		return 0;

	mhz = (double)M0_DIAG_CAL_ITERS * M0_DIAG_CAL_LOOP_CYC * (TIMER_HZ / 1e6) / d->cal_empty;
	cyc_per_count = mhz / (TIMER_HZ / 1e6);
	printf("\nCPU: %u iterations of a 4-cycle loop took %u timer counts\n",
	       M0_DIAG_CAL_ITERS, d->cal_empty);
	printf("  effective speed %.1f MHz (compare with hclk_m0 in dmesg; lower means the\n"
	       "  code is paying wait states on every fetch)\n", mhz);
	printf("Bus access cost, cycles each (2.0 = no wait states):\n");
#define COST(f) ((double)((int32_t)(d->f - d->cal_empty)) * cyc_per_count / M0_DIAG_CAL_ITERS)
	printf("  GPIO4 DR write          %5.1f\n", COST(cal_gpio_wr));
	printf("  TIMER0 status read      %5.1f\n", COST(cal_timer_rd));
	printf("  SRAM read, 0xFFF89000   %5.1f\n", COST(cal_sram_abs));
	printf("  SRAM read, low window   %5.1f\n", COST(cal_sram_low));

	if (d->period_n) {
		double per = (double)d->period_counts / d->period_n;

		printf("\nTimer period with LOAD = %u: %.3f counts per expiry -> period is LOAD%s\n",
		       M0_DIAG_PERIOD_LOAD, per,
		       per > M0_DIAG_PERIOD_LOAD + 0.5 ? " + 1" : "");
	}

	printf("\n");
	latency("Polling", &d->poll, mhz);
	printf("1 kHz polled ticks: %u of %u\n", d->poll_slow_ticks, M0_DIAG_SLOW_N);

	if (stage >= M0_DIAG_WFI_SLOW) {
		printf("\n1 kHz WFI ticks: %u of %u, WFI returns with no expiry: %u\n",
		       d->wfi_slow_ticks, M0_DIAG_SLOW_N, d->wfi_slow_spurious);
		if (d->wfi_slow_ticks < M0_DIAG_SLOW_N)
			printf("  not finished: if this number is not moving, WFI is not being woken\n"
			       "  by the masked timer interrupt (see 'M0 sleeping' above).\n");
		else if (d->wfi_slow_spurious > 10 * M0_DIAG_SLOW_N)
			printf("  WFI is returning without sleeping: it works as a slow poll only.\n");
		else
			printf("  WFI sleeps and is woken by the masked timer interrupt.\n");
		latency("WFI", &d->wfi, mhz);
		if (d->wfi.n >= M0_DIAG_LAT_N)
			printf("  NVIC line 19 was pending on %u of %u wakes\n",
			       d->wfi.pend_seen, d->wfi.n);
	}

	if (stage == M0_DIAG_RUN) {
		h0 = d->heartbeat;
		t0 = now();
		sleep(2);
		h1 = d->heartbeat;
		t1 = now();
		printf("\nHeartbeat: %.2f ticks/s against Linux's clock (1000.00 expected).\n"
		       "  The tick is the 'clk_gpll_div_100m' timer clock / %u, so that clock is\n"
		       "  %.3f MHz.\n", (h1 - h0) / (t1 - t0), M0_DIAG_SLOW_PERIOD,
		       (h1 - h0) / (t1 - t0) * M0_DIAG_SLOW_PERIOD / 1e6);
	}
	return 0;
}
