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
_Static_assert(DSM_FS_SHIFT >= M0_COMP_LOG2_CYCLES, "comp scaling");
/* m0_play_comp's m = FS * delta / tick is the count difference << 8 less FS */
_Static_assert(DSM_FS_SHIFT == M0_COMP_LOG2_CYCLES + 8, "comp scaling: SysTick's 24-bit count is shifted up by 8");
_Static_assert(M0_MIN_TICKS_PER_SAMPLE > M0_COMP_STEPS, "a sample needs a plain tick too");
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

/* SysTick as a free-running 24-bit counter (m0_play_comp): no tick, no
 * exception, a count that wraps after 2^24 cycles */
static void tick_free_run(void)
{
	REG(SYST_CSR) = 0;
	REG(SYST_RVR) = SYST_MAX;
	REG(SYST_CVR) = 0;
	tick_ack();
	REG(SYST_CSR) = SYST_CSR_FREE;
}

/* The tick length tick_start() will use (the header's, if it is usable) */
static uint32_t tick_cycles_of(const m0_audio_shmem_t *shmem)
{
	if (shmem->tick_cycles < 2U || shmem->tick_cycles > SYST_MAX + 1U ||
	    shmem->ticks_base < M0_MIN_TICKS_PER_SAMPLE)
		return M0_DEFAULT_TICK_CYCLES;
	return shmem->tick_cycles;
}

/* Take the output between "both pins low" (idle) and the mid level that
 * silence plays at (half the ticks high), or back. The buffer is AC coupled,
 * so a jump between the two is a pop; a slow smoothstep in the density of
 * ones is not: it is a few hertz, below what headphones play, and its
 * slope-limited step through the coupling capacitor is tiny. 'up' fades in.
 *
 * A first-order modulator with a little dither (so the density does not turn
 * into whistling tones at low values) at one bit per tick, the same tick as
 * the stream will use; both channels get the same bits. The density changes
 * every 2^M0_FADE_STEP_LOG2 ticks. SysTick runs free here and the loop
 * schedules its ticks, so one that runs late does not stretch the fade. */
#define M0_FADE_STEP_LOG2   6   /* ticks per density step */
#define M0_FADE_STEPS_LOG2  12  /* density steps: 2^18 ticks, 90 ms at 2.9 MHz */

static void fade_pins(uint32_t cycles, int up)
{
	uint32_t rnd = 0x1D872B41U, acc = 0, prev;
	uint32_t i, k;

	tick_free_run();
	prev = REG(SYST_CVR);
	for (i = 0; i < (1U << M0_FADE_STEPS_LOG2); i++) {
		/* smoothstep 3u^2 - 2u^3 in Q16, then half of it: density 0..0.5 */
		uint32_t u = (up ? i : (1U << M0_FADE_STEPS_LOG2) - 1U - i)
			     << (16 - M0_FADE_STEPS_LOG2);
		uint32_t u2 = (u * u) >> 16;
		uint32_t u3 = (u2 * u) >> 16;
		uint32_t dens = (3U * u2 - 2U * u3) >> 1;

		for (k = 0; k < (1U << M0_FADE_STEP_LOG2); k++) {
			uint32_t bit;

			rnd ^= rnd << 13;
			rnd ^= rnd >> 17;
			rnd ^= rnd << 5;
			acc += dens;
			/* error stays the undithered one; dither +-1/4 LSB */
			bit = (int32_t)acc + (int32_t)((rnd >> 16) & 0x7FFFU) - 0x4000 >= 0x10000;
			if (bit)
				acc -= 0x10000U;
			while (((prev - REG(SYST_CVR)) & SYST_MAX) < cycles)
				;
			prev = (prev - cycles) & SYST_MAX;
			gpio_write_both(bit, bit);
		}
	}
}

/* Take the tick timing from the header if it is usable, else the defaults,
 * hand the per-sample part to m0_play() and switch SysTick to the tick rate.
 * Returns whether the stream can be played by m0_play_comp(). */
static int tick_start(const m0_audio_shmem_t *shmem)
{
	uint32_t cycles = shmem->tick_cycles;
	uint32_t base = shmem->ticks_base;
	uint32_t frac = shmem->ticks_frac;
	uint32_t shift = 0, interp = ~0U, steps = M0_STEP_TICKS;
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
	/* The corrected loop is written for one tick length; it holds each
	 * sample and has fewer steps (play.S). */
	if ((shmem->flags & M0_FLAG_COMP) && cycles == M0_COMP_CYCLES) {
		comp = 1;
		shift = 0;
		steps = M0_COMP_STEPS;
	}
#endif
	m0_play_state[PS_FRAC / 4] = frac;
	m0_play_state[PS_PLAIN / 4] = base - steps;
	m0_play_state[PS_SHIFT / 4] = shift;
	m0_play_state[PS_DX_MASK / 4] = interp;
	if (comp)
		tick_free_run();
	else
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
		/* Pins from low to the mid level without a step (no pop) */
		REG(GRF_BASE + GRF_SOC_CON0) = GRF_CON0_MCU_UNBUFFERED;
		fade_pins(tick_cycles_of(shmem), 1);
		if (tick_start(shmem)) {
#ifndef M0_PROFILE
			/* Blocking writes: the loop times each pin write */
			REG(GRF_BASE + GRF_SOC_CON0) = GRF_CON0_MCU_UNBUFFERED;
			m0_play_comp(); /* until ctrl != PLAY */
#if M0_COMP_STATS == 1
			shmem->stat_late = m0_play_state[PS_DSUM / 4];
#elif M0_COMP_STATS == 2
			/* Counts of plain ticks late by >= 32, 64, 100 cycles: read with
			 * dd from /dev/mem at M0_TRACE_ADDR (no PROFILE build uses it) */
			{
				volatile uint32_t *h = (volatile uint32_t *)M0_TRACE_ADDR;

				h[0] = m0_play_state[PS_H32 / 4];
				h[1] = m0_play_state[PS_H64 / 4];
				h[2] = m0_play_state[PS_H100 / 4];
			}
			shmem->stat_late = M0_STAT_LATE_UNKNOWN;
#else
			shmem->stat_late = M0_STAT_LATE_UNKNOWN;
#endif
#endif
		} else {
			/* Let bus writes complete behind our back: the GPIO write
			 * in every tick then costs a few cycles instead of 32
			 * (rk3506_regs.h). */
			REG(GRF_BASE + GRF_SOC_CON0) = GRF_CON0_MCU_BUFFERABLE;
			m0_play(); /* until ctrl != PLAY */
			shmem->stat_late = M0_STAT_LATE_NONE;
		}
		/* ... and back to low the same way: the host stopped the stream,
		 * not the signal, so the pins are at the mid level, not at rest */
		REG(GRF_BASE + GRF_SOC_CON0) = GRF_CON0_MCU_UNBUFFERED;
		fade_pins(tick_cycles_of(shmem), 0);
		gpio_write_both(0, 0);
	}
}
