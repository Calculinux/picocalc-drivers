/* SPDX-License-Identifier: GPL-2.0 */
/* M0 delta-sigma audio: bit-bangs GPIO4_B2 (L) and B3 (R) from a ring in system
 * SRAM, one output bit per SysTick tick. The playback loop itself is
 * m0_play() in play.S; this file is pin/tick setup and the idle loop.
 *
 * Interrupts stay masked (PRIMASK) for good: the firmware has no handlers.
 * SysTick's pending exception only ever wakes WFI.
 *
 * Power saving: WFE when idle; host must wake M0 via GRF rxev (TRM GRF_SOC_CON37 bit 3).
 * WIC deep sleep (M0_SHMEM_FLAG_WIC_WAKE + grf_con_mcu_wicenreq, CON37 bit 5) is
 * NOT usable yet: that path sleeps in WFI, and rxev only completes WFE, so a
 * host that sets the flag would never get the M0 back without a real M0
 * interrupt (e.g. mailbox). The host driver never sets the flag today.
 * See RK3506 TRM Part 1 §4.6 (M0 status signals), GRF_SOC_CON37. */

#include "rk3506_regs.h"
#include "shmem.h"
#include "play.h"

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
_Static_assert(__builtin_offsetof(m0_audio_shmem_t, stat_min_cvr) == M0_SHMEM_STAT_MIN_CVR, "stat_min_cvr");
_Static_assert(__builtin_offsetof(m0_audio_shmem_t, stat_overruns) == M0_SHMEM_STAT_OVERRUNS, "stat_overruns");
_Static_assert(__builtin_offsetof(m0_audio_shmem_t, buffer) == M0_HEADER_SIZE, "header");
_Static_assert((M0_RING_BYTES & (M0_RING_BYTES - 1U)) == 0, "ring size must be a power of two");

/* Tick timing when the host gives none (see rk3506_regs.h) */
#define DEF_DEN   (M0_DEFAULT_TICK_CYCLES * M0_SAMPLE_RATE_HZ)
#define DEF_BASE  (M0_DEFAULT_CORE_HZ / DEF_DEN)
#define DEF_REM   (M0_DEFAULT_CORE_HZ % DEF_DEN)
_Static_assert(DEF_BASE >= M0_MIN_TICKS_PER_SAMPLE, "default tick rate too low");

__attribute__((always_inline)) static inline void gpio_write_both(uint32_t bit_l, uint32_t bit_r)
{
	REG(GPIO4_BASE + GPIO_DR_L) = GPIO4_DR_WRITE(bit_l, bit_r);
}

static void tick_stop(void)
{
	REG(SYST_CSR) = 0;
	REG(SCB_ICSR) = 1u << ICSR_PENDSTCLR_BIT;
}

/* Take the tick timing from the header if it is usable, else the defaults,
 * hand the per-sample part to m0_play() and start SysTick. */
static void tick_start(const m0_audio_shmem_t *shmem)
{
	uint32_t cycles = shmem->tick_cycles;
	uint32_t base = shmem->ticks_base;
	uint32_t rem = shmem->ticks_rem;
	uint32_t den = shmem->ticks_den;

	if (cycles < 2U || cycles > SYST_MAX + 1U || base < M0_MIN_TICKS_PER_SAMPLE ||
	    !den || rem >= den) {
		cycles = M0_DEFAULT_TICK_CYCLES;
		base = DEF_BASE;
		rem = DEF_REM;
		den = DEF_DEN;
	}
	m0_play_state[PS_TICKS_BASE / 4] = base;
	m0_play_state[PS_TICKS_REM / 4] = rem;
	m0_play_state[PS_TICKS_DEN / 4] = den;

	tick_stop();
	REG(SYST_RVR) = cycles - 1U;
	REG(SYST_CVR) = 0;
	REG(SYST_CSR) = SYST_CSR_RUN;
}

static void hardware_init(void)
{
	/* CRU: ungate GPIO4 (pclk + dbclk). Nothing else is needed: the tick is
	 * inside the core. */
	REG(CRU_BASE + CRU_GATE_CON13) = CRU_GPIO4_EN;

	/* GPIO4 B2/B3: digital mode then output, both low */
	REG(GPIO4_IOC_BASE + SARADC_CON) = SARADC_CON_B23_EN;
	REG(GPIO4_BASE + GPIO_DDR_L) = GPIO4_B23_OUT_DIR;
	gpio_write_both(0, 0);

	tick_stop();
}

int main(void)
{
	__asm volatile ("cpsid i");
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

		REG(CRU_BASE + CRU_GATE_CON13) = CRU_GPIO4_EN;
		tick_start(shmem);
		m0_play(); /* until ctrl != PLAY */
		tick_stop();
		gpio_write_both(0, 0);
	}
}
