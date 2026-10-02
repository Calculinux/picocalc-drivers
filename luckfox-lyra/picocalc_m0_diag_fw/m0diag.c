// SPDX-License-Identifier: GPL-2.0
/*
 * m0diag: read and interpret the results of the RK3506 M0 diagnostic firmware
 * (rk3506-m0-diag.elf) from Linux, through /dev/mem. Run as root.
 *
 *   m0diag            print everything
 *   m0diag -r         print the SoC registers only (works with any firmware,
 *                     or none: M0 sleep/lockup state and isolation bits)
 *   m0diag -t HZ      timer clock (clk_timer0_ch5) in Hz
 *   m0diag -c HZ      M0 core clock (hclk_m0) in Hz
 *
 * Without -t/-c both rates are taken from /sys/kernel/debug/clk/clk_summary;
 * failing that the timer rate is measured from the heartbeat against Linux's
 * clock and the core rate from the firmware's SysTick-versus-timer count.
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
#define CLK_SUMMARY      "/sys/kernel/debug/clk/clk_summary"

static int memfd;
static double timer_hz, cpu_hz;
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

/* Rate column of one clock in clk_summary, 0 if not found */
static double clk_rate(const char *name)
{
	char line[256], n[64];
	unsigned long en, prep, prot, rate;
	FILE *f = fopen(CLK_SUMMARY, "r");
	double r = 0;

	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (sscanf(line, " %63s %lu %lu %lu %lu", n, &en, &prep, &prot, &rate) == 5 &&
		    !strcmp(n, name)) {
			r = rate;
			break;
		}
	fclose(f);
	return r;
}

static const char *stage_name(uint32_t s)
{
	static const char *const names[] = {
		"(none)", "ALIVE", "CAL", "PERIOD", "POLL_LAT", "POLL_SLOW",
		"WFI_SLOW", "WFI_LAT", "SYST_PROBE", "WFI_STAMP", "ACCESS", "SYST_TICK", "RUN",
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

/* unit_hz: what one unit of the values is (timer counts or core cycles);
 * offset: value of histogram bin 0. */
static void latency(const char *name, const volatile struct m0_diag_lat *l,
		    const char *unit, double unit_hz, long offset)
{
	uint32_t n = l->n, i, shown = 0;

	if (!n) {
		printf("%s: nothing captured (returns with nothing pending: %u)\n",
		       name, l->spurious);
		return;
	}
	if (n < M0_DIAG_LAT_N)
		printf("%s: INCOMPLETE, %u of %u (returns with nothing pending: %u)\n",
		       name, n, M0_DIAG_LAT_N, l->spurious);
	printf("%s, %u samples, in %s (%.2f ns each):\n", name, n, unit, 1e9 / unit_hz);
	printf("  min %u  max %u  mean %.2f  spread %u = %.0f ns = %.1f core cycles\n",
	       l->min, l->max, (double)l->sum / n, l->max - l->min,
	       (l->max - l->min) * 1e9 / unit_hz, (l->max - l->min) * cpu_hz / unit_hz);
	printf("  histogram (value:count):");
	for (i = 0; i < M0_DIAG_HIST; i++)
		if (l->hist[i]) {
			if (shown++ % 8 == 0)
				printf("\n   ");
			printf(" %ld%s:%u", (long)i + offset,
			       i == M0_DIAG_HIST - 1 ? "+" : "", l->hist[i]);
		}
	printf("\n");
}

int main(int argc, char **argv)
{
	const volatile struct m0_diag *d;
	double t0, t1, k, hb_hz = 0;
	const char *thz_src = "-t", *chz_src = "-c";
	uint32_t stage, h0, h1;
	int regs_only = 0, i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-r"))
			regs_only = 1;
		else if (!strcmp(argv[i], "-t") && i + 1 < argc)
			timer_hz = atof(argv[++i]);
		else if (!strcmp(argv[i], "-c") && i + 1 < argc)
			cpu_hz = atof(argv[++i]);
	}

	memfd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (memfd < 0) {
		perror("/dev/mem");
		return 1;
	}

	soc_registers();
	if (regs_only)
		return 0;

	d = (const volatile struct m0_diag *)map(M0_DIAG_ADDR);
	printf("\nResults block at 0x%08x:\n", M0_DIAG_ADDR);
	if (d->magic != M0_DIAG_MAGIC) {
		printf("  no diagnostic results (magic %08x): the diagnostic firmware has not\n"
		       "  run, did not get as far as main(), or cannot write this SRAM.\n",
		       d->magic);
		return 2;
	}
	if (d->version != M0_DIAG_VERSION) {
		printf("  results are version %u, this tool reads version %u\n",
		       d->version, M0_DIAG_VERSION);
		return 2;
	}
	stage = d->stage;
	printf("  stage %u %s%s\n", stage, stage_name(stage),
	       d->fault ? "  ** HardFault during this stage **" : "");

	if (stage == M0_DIAG_RUN) {
		h0 = d->heartbeat;
		t0 = now();
		sleep(2);
		h1 = d->heartbeat;
		t1 = now();
		hb_hz = (h1 - h0) / (t1 - t0) * M0_DIAG_SLOW_PERIOD;
	}
	if (timer_hz <= 0) {
		timer_hz = clk_rate("clk_timer0_ch5");
		thz_src = "clk_summary";
	}
	if (timer_hz <= 0) {
		timer_hz = hb_hz;
		thz_src = "heartbeat";
	}
	if (timer_hz <= 0) {
		timer_hz = 100e6;
		thz_src = "ASSUMED";
	}
	if (cpu_hz <= 0) {
		cpu_hz = clk_rate("hclk_m0");
		chz_src = "clk_summary";
	}
	if (cpu_hz <= 0 && d->syst_present) {
		cpu_hz = d->syst_per_1000 / 1000.0 * timer_hz;
		chz_src = "SysTick";
	}
	if (cpu_hz <= 0) {
		cpu_hz = timer_hz * 2;
		chz_src = "ASSUMED";
	}
	printf("\nClocks: timer %.3f MHz (%s), M0 core %.3f MHz (%s)\n",
	       timer_hz / 1e6, thz_src, cpu_hz / 1e6, chz_src);
	if (hb_hz > 0)
		printf("  timer clock measured against Linux's clock: %.3f MHz\n", hb_hz / 1e6);

	if (stage <= M0_DIAG_CAL)
		return 0;
	if (!d->cal_empty) {
		printf("\nThe stopwatch (TIMER0_CH4) did not count during calibration: its clock is\n"
		       "  off. Check clk_timer0_ch4/ch5 and their parent in clk_summary (the\n"
		       "  parent must be enabled; see assigned-clock-parents on the loader node).\n");
		return 3;
	}

	k = cpu_hz / timer_hz / M0_DIAG_CAL_ITERS; /* stopwatch counts -> core cycles per iteration */
	printf("\nCost in core clock cycles (in brackets: with no wait states):\n");
	printf("  loop of subs + taken branch      %5.1f  (4)\n", d->cal_empty * k);
	printf("  one straight-line instruction    %5.2f  (1)\n",
	       (double)(int32_t)(d->cal_nop8 - d->cal_empty) * k / 8);
#define EXTRA(f) ((double)(int32_t)(d->f - d->cal_empty) * k)
	printf("  GPIO4 DR write                   %5.1f  (2)\n", EXTRA(cal_gpio_wr));
	printf("  TIMER0 status read               %5.1f  (2)\n", EXTRA(cal_timer_rd));
	printf("  core-internal (NVIC) write       %5.1f  (2)\n", EXTRA(cal_ppb_wr));
	printf("  SRAM read at 0xFFF89000          %5.1f  (2)\n", EXTRA(cal_sram_abs));
	printf("  SRAM read through low window     %5.1f  (2)\n", EXTRA(cal_sram_low));

	if (d->period_n) {
		double per = (double)d->period_counts / d->period_n;

		printf("\nTimer period with LOAD = %u: %.3f counts per expiry -> period is LOAD%s\n",
		       M0_DIAG_PERIOD_LOAD, per,
		       per > M0_DIAG_PERIOD_LOAD + 0.5 ? " + 1" : "");
	}

	printf("\n");
	latency("Polling, expiry to detect", &d->poll, "timer counts", timer_hz, 0);
	printf("slow polled ticks: %u of %u\n", d->poll_slow_ticks, M0_DIAG_SLOW_N);

	if (stage >= M0_DIAG_WFI_SLOW) {
		printf("\nslow WFI ticks: %u of %u, WFI returns with no expiry: %u\n",
		       d->wfi_slow_ticks, M0_DIAG_SLOW_N, d->wfi_slow_spurious);
		if (d->wfi_slow_ticks < M0_DIAG_SLOW_N)
			printf("  not finished: if this number is not moving, WFI is not being woken\n"
			       "  by the masked timer interrupt (see 'M0 sleeping' above).\n");
		else if (d->wfi_slow_spurious > 10 * M0_DIAG_SLOW_N)
			printf("  WFI is returning without sleeping: it works as a slow poll only.\n");
		else
			printf("  WFI sleeps and is woken by the masked timer interrupt.\n");
		latency("WFI, expiry to detect", &d->wfi, "timer counts", timer_hz, 0);
		if (d->wfi.n)
			printf("  NVIC line 19 was pending on %u of %u wakes\n",
			       d->wfi.pend_seen, d->wfi.n);
	}

	if (stage >= M0_DIAG_SYST_PROBE) {
		printf("\nSysTick: %s (SYST_CALIB %08x)\n",
		       d->syst_present ? "present" : "NOT implemented or not counting",
		       d->syst_calib);
		if (d->syst_present)
			printf("  %u core cycles per 1000 timer counts -> core clock %.3f MHz\n",
			       d->syst_per_1000, d->syst_per_1000 / 1000.0 * timer_hz / 1e6);
	}
	if (stage >= M0_DIAG_WFI_STAMP && d->syst_present) {
		printf("\n");
		latency("Timer-woken WFI, interval between wakes (SysTick stamp)", &d->stamp,
			"core cycles", cpu_hz, (long)d->stamp_first - M0_DIAG_STAMP_CENTRE);
		printf("  expected interval %.1f core cycles; the spread is the wake jitter\n",
		       M0_DIAG_LAT_PERIOD * cpu_hz / timer_hz);
	}
	if (stage >= M0_DIAG_ACCESS && d->syst_present) {
		printf("\n");
		latency("One GPIO4 DR write, start to finish", &d->acc_gpio,
			"core cycles", cpu_hz, 0);
		latency("One TIMER0 status read, start to finish", &d->acc_timer,
			"core cycles", cpu_hz, 0);
		printf("  (each includes one SysTick read, about 2 cycles; a spread of 0 means the\n"
		       "  bus adds no jitter to when a write lands)\n");
	}
	if (stage >= M0_DIAG_SYST_TICK && d->syst_present) {
		printf("\n");
		if (d->syst_nowake)
			printf("SysTick woke nothing during %u backstop periods: it does NOT wake WFI\n",
			       d->syst_nowake);
		else
			printf("SysTick wakes WFI with its exception masked.\n");
		latency("SysTick-woken WFI, reload to first instruction", &d->syst,
			"core cycles", cpu_hz, 0);
		printf("  (the few large values are ticks that followed a backstop timer wake)\n");
	}
	return 0;
}
