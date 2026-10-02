/* SPDX-License-Identifier: GPL-2.0 */
/* M0 delta-sigma audio: bit-bangs GPIO4_B2 (L) and B3 (R) from a ring in system
 * SRAM, one output bit per SysTick tick. The playback loop itself is
 * m0_play() in play.S; this file is pin/tick setup and the idle loop.
 *
 * The firmware is loaded once and never exits (in TCM mode it cannot be
 * reloaded before the next reboot). Between streams it idles: SysTick slowed
 * to one wake every few milliseconds, the core asleep in WFI in between,
 * looking at the host's control word on each wake. See shmem.h for the
 * handshake.
 *
 * Interrupts stay masked (PRIMASK) for good: the firmware has no handlers.
 * SysTick's pending exception only ever wakes WFI. */

#include "rk3506_regs.h"
#include "shmem.h"
#include "play.h"

#define REG(addr)   (*(volatile uint32_t *)(addr))

/* Offsets play.S uses; keep the struct and the assembler in step. */
_Static_assert(__builtin_offsetof(m0_audio_shmem_t, ctrl) == M0_SHMEM_CTRL, "ctrl");
_Static_assert(__builtin_offsetof(m0_audio_shmem_t, write_idx) == M0_SHMEM_WRITE_IDX, "write_idx");
_Static_assert(__builtin_offsetof(m0_audio_shmem_t, read_idx) == M0_SHMEM_READ_IDX, "read_idx");
_Static_assert(__builtin_offsetof(m0_audio_shmem_t, stat_min_cvr) == M0_SHMEM_STAT_MIN_CVR, "stat_min_cvr");
_Static_assert(__builtin_offsetof(m0_audio_shmem_t, stat_overruns) == M0_SHMEM_STAT_OVERRUNS, "stat_overruns");
_Static_assert(__builtin_offsetof(m0_audio_shmem_t, buffer) == M0_HEADER_SIZE, "header");
_Static_assert(DSM_FS_SHIFT + M0_MAX_SHIFT >= M0_COMP_LOG2_CYCLES, "comp scaling");
_Static_assert((M0_RING_BYTES & (M0_RING_BYTES - 1U)) == 0, "ring size must be a power of two");
_Static_assert(M0_IDLE_CYCLES <= SYST_MAX + 1U, "idle period does not fit SysTick");

/* Tick timing when the host gives none (see rk3506_regs.h) */
#define DEF_DEN   (M0_DEFAULT_TICK_CYCLES * M0_SAMPLE_RATE_HZ)
#define DEF_BASE  (M0_DEFAULT_CORE_HZ / DEF_DEN)
#define DEF_FRAC  ((uint32_t)(((unsigned long long)(M0_DEFAULT_CORE_HZ % DEF_DEN) << 32) / DEF_DEN))
_Static_assert(DEF_BASE >= M0_MIN_TICKS_PER_SAMPLE, "default tick rate too low");
_Static_assert(M0_MIN_TICKS_PER_SAMPLE > M0_STEP_TICKS, "a sample needs a plain tick too");

__attribute__((always_inline)) static inline void gpio_write_both(uint32_t bit_l, uint32_t bit_r)
{
	REG(GPIO4_BASE + GPIO_DR_L) = GPIO4_DR_WRITE(bit_l, bit_r);
}

__attribute__((always_inline)) static inline void tick_ack(void)
{
	REG(SCB_ICSR) = 1u << ICSR_PENDSTCLR_BIT;
}

/* One SysTick wake every 'cycles' core clock cycles */
static void tick_set(uint32_t cycles)
{
	REG(SYST_CSR) = 0;
	REG(SYST_RVR) = cycles - 1U;
	REG(SYST_CVR) = 0;
	tick_ack();
	REG(SYST_CSR) = SYST_CSR_RUN;
}

/* Take the tick timing from the header if it is usable, else the defaults,
 * hand the per-sample part to m0_play() and switch SysTick to the tick rate.
 * Returns whether the stream can be played by m0_play_comp(). */
static int tick_start(const m0_audio_shmem_t *shmem)
{
	uint32_t cycles = shmem->tick_cycles;
	uint32_t base = shmem->ticks_base;
	uint32_t frac = shmem->ticks_frac;
	uint32_t shift = 0, interp = ~0U;
	int comp = 0;

	if (cycles < 2U || cycles > SYST_MAX + 1U || base < M0_MIN_TICKS_PER_SAMPLE) {
		cycles = M0_DEFAULT_TICK_CYCLES;
		base = DEF_BASE;
		frac = DEF_FRAC;
	}
	/* The input steps 2^-shift of the way to the next sample per tick: the
	 * smallest shift that does not overshoot in the longest sample. */
	while ((1U << shift) < base + 1U)
		shift++;
	if (shift > M0_MAX_SHIFT) {
		shift = M0_MAX_SHIFT;
		interp = 0;
	}
	if (shmem->flags & M0_FLAG_NO_INTERP)
		interp = 0;
	/* Holding needs no fraction, and a held full-scale square drives the
	 * state far further than a ramped one: keep the headroom. */
	if (!interp)
		shift = 0;
#ifndef M0_PROFILE
	/* The corrected loop is written for one tick length and one shift
	 * (play.S); with those, holding has to live with the smaller headroom. */
	if ((shmem->flags & M0_FLAG_COMP) && cycles == M0_COMP_CYCLES &&
	    base + 1U <= (1U << M0_MAX_SHIFT)) {
		comp = 1;
		shift = M0_MAX_SHIFT;
	}
#endif
	m0_play_state[PS_FRAC / 4] = frac;
	m0_play_state[PS_PLAIN / 4] = base - M0_STEP_TICKS;
	m0_play_state[PS_SHIFT / 4] = shift;
	m0_play_state[PS_DX_MASK / 4] = interp;
	tick_set(cycles);
	return comp;
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
}

int main(void)
{
	m0_audio_shmem_t *shmem = (m0_audio_shmem_t *)M0_SHMEM_ADDR;

	__asm volatile ("cpsid i");
	hardware_init();

	for (;;) {
		/* Idle: asleep except for a look at ctrl every M0_IDLE_CYCLES */
		tick_set(M0_IDLE_CYCLES);
		shmem->m0_state = M0_STATE_IDLE;
		do {
			__asm volatile ("wfi");
			tick_ack();
		} while (shmem->magic != M0_AUDIO_MAGIC || shmem->ctrl != M0_CTRL_PLAY);

		shmem->m0_state = M0_STATE_PLAY;
		REG(CRU_BASE + CRU_GATE_CON13) = CRU_GPIO4_EN;
		if (tick_start(shmem)) {
#ifndef M0_PROFILE
			/* Blocking writes: the loop times each pin write */
			REG(GRF_BASE + GRF_SOC_CON0) = GRF_CON0_MCU_UNBUFFERED;
			m0_play_comp(); /* until ctrl != PLAY */
			shmem->stat_late = m0_play_state[PS_DSUM / 4];
#endif
		} else {
			/* Let bus writes complete behind our back: the GPIO write
			 * in every tick then costs a few cycles instead of 32
			 * (rk3506_regs.h). */
			REG(GRF_BASE + GRF_SOC_CON0) = GRF_CON0_MCU_BUFFERABLE;
			m0_play(); /* until ctrl != PLAY */
			shmem->stat_late = M0_STAT_LATE_NONE;
		}
		gpio_write_both(0, 0);
	}
}
