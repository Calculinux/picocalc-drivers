/* SPDX-License-Identifier: GPL-2.0 */
/* Results block shared by the M0 diagnostic firmware and the m0diag host tool.
 * Lives in system SRAM right after the firmware image, the same place the
 * audio firmware keeps its ring header; the magic tells them apart. */

#ifndef M0_DIAG_H
#define M0_DIAG_H

#include <stdint.h>

#define M0_DIAG_ADDR      0xFFF89000U
#define M0_DIAG_MAGIC     0x4D304447U  /* "M0DG" */
#define M0_DIAG_VERSION   3U

/* The firmware writes the stage BEFORE starting it, so a hang or fault
 * leaves the stage it happened in. */
enum m0_diag_stage {
	M0_DIAG_ALIVE     = 1,  /* reached main(), clocks and pins set up */
	M0_DIAG_CAL       = 2,  /* stopwatch loops: CPU speed and bus access cost */
	M0_DIAG_PERIOD    = 3,  /* timer period in counts for a known LOAD */
	M0_DIAG_POLL_LAT  = 4,  /* expiry-to-detect latency, polling */
	M0_DIAG_POLL_SLOW = 5,  /* 1 kHz tick by polling, B2 toggles */
	M0_DIAG_WFI_SLOW  = 6,  /* 1 kHz tick by WFI, B2 toggles */
	M0_DIAG_WFI_LAT   = 7,  /* expiry-to-detect latency, WFI */
	M0_DIAG_SYST_PROBE = 8, /* is the core's SysTick timer implemented? */
	M0_DIAG_WFI_STAMP = 9,  /* timer-woken WFI, wake time stamped with SysTick */
	M0_DIAG_ACCESS    = 10, /* how long one GPIO write / timer read takes */
	M0_DIAG_SYST_TICK = 11, /* SysTick as the tick: WFI wake latency */
	M0_DIAG_RUN       = 12, /* tests done: slow WFI heartbeat forever */
};

/* Sizes of the tests (the host tool needs them to interpret the numbers) */
#define M0_DIAG_CAL_ITERS      65536U
#define M0_DIAG_CAL_LOOP_CYC   4U       /* subs + taken bne */
#define M0_DIAG_PERIOD_LOAD    999U
#define M0_DIAG_PERIOD_N       10000U
#define M0_DIAG_LAT_PERIOD     1000U    /* timer counts between latency ticks */
#define M0_DIAG_LAT_N          4096U
#define M0_DIAG_SLOW_PERIOD    100000U  /* timer counts between slow ticks */
#define M0_DIAG_SLOW_N         1000U
#define M0_DIAG_SYST_PERIOD    2000U    /* core clock cycles between SysTick ticks */
#define M0_DIAG_HIST           64U
#define M0_DIAG_STAMP_CENTRE   32U      /* histogram bin of the first interval */

struct m0_diag_lat {
	uint32_t n;         /* ticks captured so far (live) */
	uint32_t min;
	uint32_t max;
	uint32_t sum;
	uint32_t spurious;  /* WFI returned with nothing pending (live) */
	uint32_t pend_seen; /* wakes where the expected pending bit was set */
	uint32_t hist[M0_DIAG_HIST]; /* value, clamped to the last bin */
};

struct m0_diag {
	uint32_t magic;
	uint32_t version;
	uint32_t stage;         /* enum m0_diag_stage */
	uint32_t fault;         /* nonzero: HardFault taken during 'stage' */
	uint32_t heartbeat;     /* ticks counted in M0_DIAG_RUN */

	/* M0_DIAG_CAL: stopwatch counts for M0_DIAG_CAL_ITERS iterations */
	uint32_t cal_empty;     /* subs; bne */
	uint32_t cal_nop8;      /* + 8 nops: cost of straight-line code */
	uint32_t cal_gpio_wr;   /* + str to GPIO4 DR */
	uint32_t cal_timer_rd;  /* + ldr from TIMER0_CH5 INTSTAT */
	uint32_t cal_sram_abs;  /* + ldr from SRAM at its absolute address */
	uint32_t cal_sram_low;  /* + ldr from SRAM through the address-0 window */
	uint32_t cal_ppb_wr;    /* + str to a core-internal register (NVIC) */

	/* M0_DIAG_PERIOD: stopwatch counts across period_n expiries with
	 * LOAD = M0_DIAG_PERIOD_LOAD. counts/n = LOAD + 1 or LOAD. */
	uint32_t period_n;
	uint32_t period_counts;

	uint32_t poll_slow_ticks;   /* live */
	uint32_t wfi_slow_ticks;    /* live */
	uint32_t wfi_slow_spurious; /* live */

	/* SysTick */
	uint32_t syst_present;  /* 1: counts on the core clock */
	uint32_t syst_calib;    /* SYST_CALIB as read */
	uint32_t syst_per_1000; /* core cycles per 1000 timer counts (stopwatch) */
	uint32_t stamp_first;   /* first wake-to-wake interval, core cycles */
	uint32_t syst_nowake;   /* SYST_TICK: backstop periods with no SysTick wake */

	/* Timer counts from expiry to the read after detection */
	struct m0_diag_lat poll;
	struct m0_diag_lat wfi;
	/* Core cycles between consecutive timer-woken WFI wakes. min/max/sum are
	 * the intervals; hist is (interval - stamp_first + M0_DIAG_STAMP_CENTRE). */
	struct m0_diag_lat stamp;
	/* Core cycles from SysTick reload to the first instruction after WFI */
	struct m0_diag_lat syst;
	/* Core cycles one access takes, SysTick read before and after, sampled
	 * at a different point of the timer period each time */
	struct m0_diag_lat acc_gpio;   /* str to GPIO4 DR */
	struct m0_diag_lat acc_timer;  /* ldr from TIMER0_CH5 INTSTAT */
};

#endif /* M0_DIAG_H */
