/* SPDX-License-Identifier: GPL-2.0 */
/* M0 delta-sigma audio: bit-bangs GPIO4_B2 (L) and B3 (R) from a ring in system
 * SRAM, one output bit per TIMER0_CH5 expiry. The playback loop itself is
 * m0_play() in play.S; this file is clock/pin/timer setup and the idle loop.
 *
 * Power saving: WFE when idle; host must wake M0 via GRF rxev (TRM GRF_SOC_CON37 bit 3).
 * WIC deep sleep (M0_SHMEM_FLAG_WIC_WAKE + grf_con_mcu_wicenreq, CON37 bit 5) is
 * NOT usable yet: that path sleeps in WFI, and rxev only completes WFE, so a
 * host that sets the flag would never get the M0 back without a real M0
 * interrupt (e.g. mailbox). The host driver never sets the flag today.
 * See RK3506 TRM Part 1 §4.6 (M0 status signals), GRF_SOC_CON37. */

#include "rk3506_regs.h"
#include "shmem.h"

#define REG(addr)   (*(volatile uint32_t *)(addr))

/* Host driver must assert GRF rxev to wake M0: write GRF_SOC_CON37 (GRF_BASE+0x94) with
 * (GRF_CON37_WREN(GRF_CON37_RXEV_BIT) | (1u<<GRF_CON37_RXEV_BIT)), then clear rxev.
 * For WIC deep sleep set CON37 bit 5 (wicenreq) and shmem->flags M0_SHMEM_FLAG_WIC_WAKE. */
#define __WFI()            __asm volatile ("wfi")
#define __WFE()            __asm volatile ("wfe")

/* Offsets play.S uses; keep the struct and the assembler in step. */
_Static_assert(__builtin_offsetof(m0_audio_shmem_t, ctrl) == M0_SHMEM_CTRL, "ctrl");
_Static_assert(__builtin_offsetof(m0_audio_shmem_t, write_idx) == M0_SHMEM_WRITE_IDX, "write_idx");
_Static_assert(__builtin_offsetof(m0_audio_shmem_t, read_idx) == M0_SHMEM_READ_IDX, "read_idx");
_Static_assert(__builtin_offsetof(m0_audio_shmem_t, buffer) == M0_HEADER_SIZE, "header");
_Static_assert((M0_RING_BYTES & (M0_RING_BYTES - 1U)) == 0, "ring size must be a power of two");
_Static_assert(DS_RATE_HZ * DS_PERIOD_TICKS == 100000000U, "DS_RATE_HZ must match DS_PERIOD_TICKS");

/* Playback loop (play.S): returns when shmem->ctrl leaves M0_CTRL_PLAY. */
void m0_play(void);

__attribute__((always_inline)) static inline void gpio_write_both(uint32_t bit_l, uint32_t bit_r)
{
	REG(GPIO4_BASE + GPIO_DR_L) = GPIO4_DR_WRITE(bit_l, bit_r);
}

__attribute__((always_inline)) static inline void clear_timer5_irq(void)
{
	REG(TIMER0_CH5_BASE + TIMER_INTSTAT) = 1;
}

static void timer5_start(void)
{
	/* TRM 10.3.2 sequence: program LOAD with the channel disabled, then enable. */
	REG(TIMER0_CH5_BASE + TIMER_CTRL) = TIMER_STOP;
	REG(TIMER0_CH5_BASE + TIMER_LOAD0) = DS_TIMER_LOAD;
	REG(TIMER0_CH5_BASE + TIMER_LOAD1) = 0;
	clear_timer5_irq();
	REG(TIMER0_CH5_BASE + TIMER_CTRL) = TIMER_RUN;
}

static void timer5_stop(void)
{
	REG(TIMER0_CH5_BASE + TIMER_CTRL) = TIMER_STOP;
	clear_timer5_irq();
}

#ifdef M0_TICK_WFI
/*
 * WFI build: the timer interrupt is enabled in the NVIC but never taken.
 * With PRIMASK set a pending interrupt still wakes the core from WFI, so
 * m0_play() can sleep between ticks without paying for exception entry/exit.
 * PRIMASK stays set for good: this firmware has no handlers to run.
 */
static void tick_irq_arm(void)
{
	__asm volatile ("cpsid i");
	REG(NVIC_ICPR0) = (1u << TIMER0_CH5_IRQ);
	REG(NVIC_ISER0) = (1u << TIMER0_CH5_IRQ);
}

static void tick_irq_disarm(void)
{
	REG(NVIC_ICER0) = (1u << TIMER0_CH5_IRQ);
	REG(NVIC_ICPR0) = (1u << TIMER0_CH5_IRQ);
}
#else
/* Polling build: m0_play() reads the timer's status bit; the NVIC is unused. */
static void tick_irq_arm(void) { }
static void tick_irq_disarm(void) { }
#endif

static void clocks_ungate_play(void)
{
	/* CRU: ungate pclk_timer0 + clk_timer0_ch5, 100 MHz on ch5, ungate GPIO4 */
	REG(CRU_BASE + CRU_GATE_CON06) = CRU_TIMER5_EN;
	REG(CRU_BASE + CRU_CLKSEL_CON23) = CRU_TIMER5_100M;
	REG(CRU_BASE + CRU_GATE_CON13) = CRU_GPIO4_EN;
}

static void hardware_init(void)
{
	clocks_ungate_play();

	/* GPIO4 B2/B3: digital mode then output, both low */
	REG(GPIO4_IOC_BASE + SARADC_CON) = SARADC_CON_B23_EN;
	REG(GPIO4_BASE + GPIO_DDR_L) = GPIO4_B23_OUT_DIR;
	gpio_write_both(0, 0);

	/* Timer: stopped, no stale interrupt (the channel keeps its state across
	 * an M0 reset, so a previous run may have left it armed) */
	timer5_stop();
}

/*
 * Gate TIMER5/GPIO4 clocks when idle to save power. Intentionally a stub:
 * with the current Linux integration we always rproc_shutdown() on stop, so
 * the M0 is never kept alive between play cycles and this path has no effect.
 * If we move to a "keep M0 alive" model, implement actual gate-disable values
 * per TRM for CRU_GATE_CON06 / CRU_GATE_CON13; those registers may be shared
 * with other peripherals — use read-modify-write if needed.
 */
static void clocks_gate_idle(void)
{
}

int main(void)
{
	hardware_init();

	for (;;) {
		m0_audio_shmem_t *shmem = (m0_audio_shmem_t *)M0_SHMEM_ADDR;

		while (shmem->magic != M0_AUDIO_MAGIC)
			__WFE();
		if (shmem->flags & M0_SHMEM_FLAG_WIC_WAKE) {
			REG(SCB_SCR) = SCB_SCR_SLEEPDEEP;
			while (shmem->ctrl != M0_CTRL_PLAY)
				__WFI();
			REG(SCB_SCR) = 0;
		} else {
			while (shmem->ctrl != M0_CTRL_PLAY)
				__WFE();
		}

		clocks_ungate_play();
		tick_irq_arm();
		timer5_start();
		m0_play(); /* until ctrl != PLAY */
		timer5_stop();
		tick_irq_disarm();
		gpio_write_both(0, 0);
		clocks_gate_idle();
	}
}
