/* SPDX-License-Identifier: GPL-2.0 */
/* RK3506 Cortex-M0 bring-up diagnostics. Not the audio firmware: load this
 * through the same remoteproc to find out, on a real board, the things the
 * audio firmware's timing depends on. Results go to a block in system SRAM
 * that the m0diag tool reads from Linux (see README.md).
 *
 * In order (the stage is recorded before each one, so a hang or fault names
 * the stage it happened in):
 *   CAL        effective CPU speed and the cost of each kind of bus access
 *   PERIOD     timer counts per expiry for a known LOAD
 *   POLL_LAT   expiry-to-detect latency when polling the timer status
 *   POLL_SLOW  1 kHz tick by polling, GPIO4_B2 toggles (500 Hz on pin 31)
 *   WFI_SLOW   the same by WFI with interrupts masked
 *   WFI_LAT    expiry-to-detect latency with WFI
 *   RUN        1 kHz WFI heartbeat forever, B2 keeps toggling
 * Everything that might not work on this SoC (WFI wake) comes after
 * everything that only needs the timer.
 */

#include "rk3506_regs.h"
#include "diag.h"

#define REG(addr)   (*(volatile uint32_t *)(addr))

/* TIMER0_CH4 is the stopwatch (free-running, no interrupt); CH5 is the tick,
 * as in the audio firmware. Both run from clk_gpll_div_100m.
 * GATE_CON06: bit 2 pclk_timer0, bit 7 clk_timer0_ch4, bit 8 clk_timer0_ch5.
 * CLKSEL_CON23: [5:3] ch4_sel, [8:6] ch5_sel, 0b001 = clk_gpll_div_100m. */
#define TIMER0_CH4_BASE   0xFF254000U
#define TIMER_CURR0       0x0008U
#define TIMER_FREE_NOINT  0x01U
#define CRU_TIMER45_EN    0x01840000U
#define CRU_TIMER45_100M  0x01F80048U
#define NVIC_ISPR0        0xE000E200U
#define TICK_IRQ_BIT      (1u << TIMER0_CH5_IRQ)

void diag_loop_empty(uint32_t n);
void diag_loop_str(uint32_t n, volatile uint32_t *addr, uint32_t val);
void diag_loop_ldr(uint32_t n, volatile uint32_t *addr);
uint32_t diag_wait_poll(uint32_t timer_base);
uint32_t diag_wait_wfi(uint32_t timer_base);

static volatile struct m0_diag *const res = (volatile struct m0_diag *)M0_DIAG_ADDR;

/* Read through the window the M0 sees at address 0 (this variable is in .data) */
static volatile uint32_t low_word = 1;

void HardFault_Handler(void)
{
	res->fault = 1;
	for (;;)
		;
}

static void stopwatch_start(void)
{
	REG(TIMER0_CH4_BASE + TIMER_CTRL) = TIMER_STOP;
	REG(TIMER0_CH4_BASE + TIMER_LOAD0) = 0xFFFFFFFFU;
	REG(TIMER0_CH4_BASE + TIMER_LOAD1) = 0;
	REG(TIMER0_CH4_BASE + TIMER_CTRL) = TIMER_FREE_NOINT;
}

static uint32_t stopwatch(void)
{
	return REG(TIMER0_CH4_BASE + TIMER_CURR0);
}

/* Free-running tick with interrupt status; the PERIOD stage measures whether
 * the period is LOAD + 1 counts (assumed elsewhere) or LOAD. */
static void tick_start(uint32_t load)
{
	REG(TIMER0_CH5_BASE + TIMER_CTRL) = TIMER_STOP;
	REG(TIMER0_CH5_BASE + TIMER_LOAD0) = load;
	REG(TIMER0_CH5_BASE + TIMER_LOAD1) = 0;
	REG(TIMER0_CH5_BASE + TIMER_INTSTAT) = 1;
	REG(NVIC_ICPR0) = TICK_IRQ_BIT;
	REG(TIMER0_CH5_BASE + TIMER_CTRL) = TIMER_RUN;
}

static void set_b2(uint32_t level)
{
	REG(GPIO4_BASE + GPIO_DR_L) = GPIO4_DR_WRITE(level & 1u, 0u);
}

static void calibrate(void)
{
	uint32_t t;

	stopwatch_start();

	t = stopwatch();
	diag_loop_empty(M0_DIAG_CAL_ITERS);
	res->cal_empty = stopwatch() - t;

	t = stopwatch();
	diag_loop_str(M0_DIAG_CAL_ITERS, (volatile uint32_t *)(GPIO4_BASE + GPIO_DR_L),
		      GPIO4_DR_WRITE(0u, 0u));
	res->cal_gpio_wr = stopwatch() - t;

	t = stopwatch();
	diag_loop_ldr(M0_DIAG_CAL_ITERS, (volatile uint32_t *)(TIMER0_CH5_BASE + TIMER_INTSTAT));
	res->cal_timer_rd = stopwatch() - t;

	t = stopwatch();
	diag_loop_ldr(M0_DIAG_CAL_ITERS, (volatile uint32_t *)&res->magic);
	res->cal_sram_abs = stopwatch() - t;

	t = stopwatch();
	diag_loop_ldr(M0_DIAG_CAL_ITERS, &low_word);
	res->cal_sram_low = stopwatch() - t;
}

static void period(void)
{
	uint32_t t, i;

	tick_start(M0_DIAG_PERIOD_LOAD);
	t = stopwatch();
	for (i = 0; i < M0_DIAG_PERIOD_N; i++)
		diag_wait_poll(TIMER0_CH5_BASE);
	res->period_counts = stopwatch() - t;
	res->period_n = M0_DIAG_PERIOD_N;
}

static void latency(volatile struct m0_diag_lat *l, int wfi)
{
	uint32_t n = 0, min = 0xFFFFFFFFU, max = 0, sum = 0, spurious = 0, pend = 0;

	tick_start(M0_DIAG_LAT_PERIOD - 1U);
	while (n < M0_DIAG_LAT_N) {
		uint32_t v = wfi ? diag_wait_wfi(TIMER0_CH5_BASE)
				 : diag_wait_poll(TIMER0_CH5_BASE);

		if (v == 0xFFFFFFFFU) {
			l->spurious = ++spurious;
			continue;
		}
		if (wfi) {
			if (REG(NVIC_ISPR0) & TICK_IRQ_BIT)
				pend++;
			REG(NVIC_ICPR0) = TICK_IRQ_BIT;
		}
		if (v < min)
			min = v;
		if (v > max)
			max = v;
		sum += v;
		l->n = ++n;
	}
	l->min = min;
	l->max = max;
	l->sum = sum;
	l->pend_seen = pend;
}

/* One 1 kHz tick by WFI; counts WFI returns that had no expiry behind them */
static void wfi_tick(volatile uint32_t *spurious)
{
	while (diag_wait_wfi(TIMER0_CH5_BASE) == 0xFFFFFFFFU)
		if (spurious)
			(*spurious)++;
	REG(NVIC_ICPR0) = TICK_IRQ_BIT;
}

int main(void)
{
	uint32_t i;

	res->magic = 0;
	res->version = M0_DIAG_VERSION;
	res->fault = 0;
	res->heartbeat = 0;
	res->poll_slow_ticks = 0;
	res->wfi_slow_ticks = 0;
	res->wfi_slow_spurious = 0;
	res->poll.n = res->poll.spurious = 0;
	res->wfi.n = res->wfi.spurious = 0;

	REG(CRU_BASE + CRU_GATE_CON06) = CRU_TIMER45_EN;
	REG(CRU_BASE + CRU_CLKSEL_CON23) = CRU_TIMER45_100M;
	REG(CRU_BASE + CRU_GATE_CON13) = CRU_GPIO4_EN;
	REG(GPIO4_IOC_BASE + SARADC_CON) = SARADC_CON_B23_EN;
	REG(GPIO4_BASE + GPIO_DDR_L) = GPIO4_B23_OUT_DIR;
	set_b2(0);

	res->stage = M0_DIAG_ALIVE;
	res->magic = M0_DIAG_MAGIC;

	res->stage = M0_DIAG_CAL;
	calibrate();

	res->stage = M0_DIAG_PERIOD;
	period();

	res->stage = M0_DIAG_POLL_LAT;
	latency(&res->poll, 0);

	res->stage = M0_DIAG_POLL_SLOW;
	tick_start(M0_DIAG_SLOW_PERIOD - 1U);
	for (i = 0; i < M0_DIAG_SLOW_N; i++) {
		diag_wait_poll(TIMER0_CH5_BASE);
		set_b2(i);
		res->poll_slow_ticks = i + 1U;
	}

	/* From here on the timer interrupt is enabled in the NVIC but masked by
	 * PRIMASK: it can wake WFI and is never taken. */
	__asm volatile ("cpsid i");
	REG(NVIC_ICPR0) = TICK_IRQ_BIT;
	REG(NVIC_ISER0) = TICK_IRQ_BIT;

	res->stage = M0_DIAG_WFI_SLOW;
	tick_start(M0_DIAG_SLOW_PERIOD - 1U);
	for (i = 0; i < M0_DIAG_SLOW_N; i++) {
		wfi_tick(&res->wfi_slow_spurious);
		set_b2(i);
		res->wfi_slow_ticks = i + 1U;
	}

	res->stage = M0_DIAG_WFI_LAT;
	latency(&res->wfi, 1);

	res->stage = M0_DIAG_RUN;
	tick_start(M0_DIAG_SLOW_PERIOD - 1U);
	for (i = 0;; i++) {
		wfi_tick(0);
		set_b2(i);
		res->heartbeat = i + 1U;
	}
}
