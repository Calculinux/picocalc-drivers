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
 *   POLL_SLOW  slow tick by polling, GPIO4_B2 toggles (a tone on pin 31)
 *   WFI_SLOW   the same by WFI with interrupts masked
 *   WFI_LAT    expiry-to-detect latency with WFI
 *   SYST_PROBE whether the core has a SysTick timer, and its rate
 *   WFI_STAMP  timer-woken WFI again, each wake time-stamped with SysTick
 *   ACCESS     how many core cycles one GPIO write and one timer read take
 *   SYST_TICK  SysTick as the tick source: WFI wake latency
 *   RUN        slow WFI heartbeat forever, pin quiet
 * Everything that might not work on this SoC comes after everything that
 * only needs the timer, and the SysTick stages cannot hang: a TIMER0 tick
 * runs alongside as a backstop.
 */

#include "rk3506_regs.h"
#include "diag.h"

#define REG(addr)   (*(volatile uint32_t *)(addr))

/* TIMER0_CH4 is the stopwatch (free-running, no interrupt); CH5 is the tick,
 * as in the audio firmware. Their clock source is whatever the device tree
 * assigned (clocks/assigned-clock-parents on the loader node): Linux has to
 * select and enable it, so the firmware leaves the mux alone and only makes
 * sure the gates it needs are open.
 * GATE_CON06: bit 2 pclk_timer0, bit 7 clk_timer0_ch4, bit 8 clk_timer0_ch5. */
#define TIMER0_CH4_BASE   0xFF254000U
#define TIMER_CURR0       0x0008U
#define TIMER_FREE_NOINT  0x01U
#define CRU_TIMER45_EN    0x01840000U
#define NVIC_ISPR0        0xE000E200U
#define TICK_IRQ_BIT      (1u << TIMER0_CH5_IRQ)

/* SysTick (core-internal; optional on a Cortex-M0) and the SCB pending bits */
#define SYST_CALIB        0xE000E01CU
#define SYST_ENABLE       (1u << 0)
#define SYST_TICKINT      (1u << 1)
#define SYST_CLK_CORE     (1u << 2)
#define SYST_MASK         0x00FFFFFFU
#define ICSR_PENDSTSET    (1u << 26)
#define ICSR_PENDSTCLR    (1u << 25)

void diag_loop_empty(uint32_t n);
void diag_loop_nop8(uint32_t n);
void diag_loop_str(uint32_t n, volatile uint32_t *addr, uint32_t val);
void diag_loop_ldr(uint32_t n, volatile uint32_t *addr);
uint32_t diag_wait_poll(uint32_t timer_base);
uint32_t diag_wait_wfi(uint32_t timer_base);
uint32_t diag_wfi_read(volatile uint32_t *reg);
uint32_t diag_time_str(volatile uint32_t *cvr, volatile uint32_t *addr, uint32_t val);
uint32_t diag_time_ldr(volatile uint32_t *cvr, volatile uint32_t *addr);
uint32_t diag_time_str_ldr(volatile uint32_t *cvr, volatile uint32_t *addr, uint32_t val);
uint32_t diag_time_str_str(volatile uint32_t *cvr, volatile uint32_t *addr, uint32_t val);

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
	/* The count read back lags the counter by a few timer clocks. With
	 * bufferable bus writes a read can follow the restart closely enough
	 * to see the old count; wait until the new one shows. */
	while (REG(TIMER0_CH4_BASE + TIMER_CURR0) > 0x00100000U)
		;
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

static void lat_add(volatile struct m0_diag_lat *l, uint32_t v, uint32_t bin)
{
	if (!l->n || v < l->min)
		l->min = v;
	if (v > l->max)
		l->max = v;
	l->sum += v;
	l->hist[bin < M0_DIAG_HIST ? bin : M0_DIAG_HIST - 1U]++;
	l->n++;
}

static void calibrate(void)
{
	uint32_t t;

	stopwatch_start();

	t = stopwatch();
	diag_loop_empty(M0_DIAG_CAL_ITERS);
	res->cal_empty = stopwatch() - t;

	t = stopwatch();
	diag_loop_nop8(M0_DIAG_CAL_ITERS);
	res->cal_nop8 = stopwatch() - t;

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

	/* Clearing the pending bit of a line that is not enabled: harmless */
	t = stopwatch();
	diag_loop_str(M0_DIAG_CAL_ITERS, (volatile uint32_t *)NVIC_ICPR0, TICK_IRQ_BIT);
	res->cal_ppb_wr = stopwatch() - t;
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
	tick_start(M0_DIAG_LAT_PERIOD - 1U);
	while (l->n < M0_DIAG_LAT_N) {
		uint32_t v = wfi ? diag_wait_wfi(TIMER0_CH5_BASE)
				 : diag_wait_poll(TIMER0_CH5_BASE);

		if (v == 0xFFFFFFFFU) {
			l->spurious++;
			continue;
		}
		if (wfi) {
			if (REG(NVIC_ISPR0) & TICK_IRQ_BIT)
				l->pend_seen++;
			REG(NVIC_ICPR0) = TICK_IRQ_BIT;
		}
		lat_add(l, v, v);
	}
}

/* One slow tick by WFI; counts WFI returns that had no expiry behind them */
static void wfi_tick(volatile uint32_t *spurious)
{
	while (diag_wait_wfi(TIMER0_CH5_BASE) == 0xFFFFFFFFU)
		if (spurious)
			(*spurious)++;
	REG(NVIC_ICPR0) = TICK_IRQ_BIT;
}

/* SysTick on the core clock, counting down from the top, no exception */
static int systick_probe(void)
{
	uint32_t a, b, t;

	res->syst_calib = REG(SYST_CALIB);
	REG(SYST_CSR) = 0;
	REG(SYST_RVR) = SYST_MASK;
	REG(SYST_CVR) = 0;
	REG(SYST_CSR) = SYST_ENABLE | SYST_CLK_CORE;

	/* How many core cycles pass in 1000 timer counts */
	t = stopwatch();
	a = REG(SYST_CVR);
	while (stopwatch() - t < 1000U)
		;
	b = REG(SYST_CVR);
	res->syst_per_1000 = (a - b) & SYST_MASK;
	res->syst_present = a != b;
	return a != b;
}

/* Timer-woken WFI, as in WFI_LAT, but the wake is stamped with SysTick, which
 * the core reads without going through any bus bridge. Consecutive wakes
 * should be a constant number of core cycles apart; the spread is the wake
 * jitter itself, free of the timer-read path. */
static void wfi_stamp(volatile struct m0_diag_lat *l)
{
	uint32_t prev = 0, have_prev = 0;

	tick_start(M0_DIAG_LAT_PERIOD - 1U);
	while (l->n < M0_DIAG_LAT_N) {
		uint32_t now = diag_wfi_read((volatile uint32_t *)SYST_CVR);
		uint32_t d;

		if (!REG(TIMER0_CH5_BASE + TIMER_INTSTAT)) {
			l->spurious++;
			continue;
		}
		REG(TIMER0_CH5_BASE + TIMER_INTSTAT) = 1;
		REG(NVIC_ICPR0) = TICK_IRQ_BIT;
		d = (prev - now) & SYST_MASK;
		prev = now;
		if (!have_prev) {
			have_prev = 1;
			continue;
		}
		if (!res->stamp_first)
			res->stamp_first = d;
		lat_add(l, d, d - res->stamp_first + M0_DIAG_STAMP_CENTRE);
	}
}

/* Duration of one peripheral access, stamped with SysTick either side. Taken
 * right after a timer-woken WFI, 1000 timer counts apart, so the samples
 * land on every phase of anything in the bus path with a period that does
 * not divide 1000 counts. A constant duration means a write takes effect a
 * constant time after the instruction: no jitter added by the bus. */
static void access_time(void)
{
	volatile uint32_t *cvr = (volatile uint32_t *)SYST_CVR;
	volatile uint32_t *gpio = (volatile uint32_t *)(GPIO4_BASE + GPIO_DR_L);
	uint32_t v;

	res->grf_soc_con0 = REG(GRF_BASE);
	tick_start(M0_DIAG_LAT_PERIOD - 1U);
	while (res->acc_gpio.n < M0_DIAG_LAT_N) {
		if (diag_wait_wfi(TIMER0_CH5_BASE) == 0xFFFFFFFFU)
			continue;
		REG(NVIC_ICPR0) = TICK_IRQ_BIT;
		v = diag_time_str(cvr, (volatile uint32_t *)(GPIO4_BASE + GPIO_DR_L),
				  GPIO4_DR_WRITE(0u, 0u)) & SYST_MASK;
		lat_add(&res->acc_gpio, v, v);
		v = diag_time_ldr(cvr, (volatile uint32_t *)(TIMER0_CH5_BASE + TIMER_INTSTAT)) & SYST_MASK;
		lat_add(&res->acc_timer, v, v);
		v = diag_time_ldr(cvr, gpio) & SYST_MASK;
		lat_add(&res->acc_gpio_rd, v, v);
		v = diag_time_str_ldr(cvr, gpio, GPIO4_DR_WRITE(0u, 0u)) & SYST_MASK;
		lat_add(&res->acc_gpio_wr_rd, v, v);
		v = diag_time_str_str(cvr, gpio, GPIO4_DR_WRITE(0u, 0u)) & SYST_MASK;
		lat_add(&res->acc_gpio_wr_wr, v, v);
	}
}

/* SysTick as the tick: its exception is left pending (PRIMASK is set) and
 * wakes WFI. TIMER0_CH5 ticks every ~10 ms as a backstop, so a core where
 * SysTick does not wake WFI is reported instead of hanging here. */
static void systick_tick(volatile struct m0_diag_lat *l)
{
	uint32_t since_backstop = 0;

	tick_start(1000000U - 1U);
	REG(SYST_CSR) = 0;
	REG(SYST_RVR) = M0_DIAG_SYST_PERIOD - 1U;
	REG(SYST_CVR) = 0;
	REG(SCB_ICSR) = ICSR_PENDSTCLR;
	REG(SYST_CSR) = SYST_ENABLE | SYST_TICKINT | SYST_CLK_CORE;

	while (l->n < M0_DIAG_LAT_N && res->syst_nowake < 16U) {
		uint32_t cvr = diag_wfi_read((volatile uint32_t *)SYST_CVR);
		uint32_t backstop = REG(NVIC_ISPR0) & TICK_IRQ_BIT;

		if (REG(SCB_ICSR) & ICSR_PENDSTSET) {
			REG(SCB_ICSR) = ICSR_PENDSTCLR;
			/* With the backstop pending too, this wake may be its doing:
			 * a SysTick that sets its pending bit but wakes nothing
			 * would otherwise pass for one that does. */
			if (!backstop) {
				l->pend_seen++;
				since_backstop++;
				cvr = (M0_DIAG_SYST_PERIOD - 1U) - cvr;
				lat_add(l, cvr, cvr);
			}
		} else if (!backstop) {
			l->spurious++;
		}
		/* The backstop also wakes WFI. It only counts against SysTick if
		 * SysTick woke nothing in between. */
		if (backstop) {
			REG(TIMER0_CH5_BASE + TIMER_INTSTAT) = 1;
			REG(NVIC_ICPR0) = TICK_IRQ_BIT;
			if (!since_backstop)
				res->syst_nowake++;
			since_backstop = 0;
		}
	}
	REG(SYST_CSR) = 0;
	REG(SCB_ICSR) = ICSR_PENDSTCLR;
}

int main(void)
{
	uint32_t i;

	/* SRAM keeps whatever was there: clear the whole block, magic first */
	res->magic = 0;
	for (i = 0; i < sizeof(*res) / sizeof(uint32_t); i++)
		((volatile uint32_t *)res)[i] = 0;
	res->version = M0_DIAG_VERSION;

	REG(CRU_BASE + CRU_GATE_CON06) = CRU_TIMER45_EN;
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
	set_b2(0); /* the two slow stages are the audible part; quiet from here */

	res->stage = M0_DIAG_WFI_LAT;
	latency(&res->wfi, 1);

	res->stage = M0_DIAG_SYST_PROBE;
	if (systick_probe()) {
		res->stage = M0_DIAG_WFI_STAMP;
		wfi_stamp(&res->stamp);

		res->stage = M0_DIAG_ACCESS;
		access_time();

		res->stage = M0_DIAG_SYST_TICK;
		systick_tick(&res->syst);
	}

	res->stage = M0_DIAG_RUN;
	tick_start(M0_DIAG_SLOW_PERIOD - 1U);
	for (i = 0;; i++) {
		wfi_tick(0);
		res->heartbeat = i + 1U;
	}
}
