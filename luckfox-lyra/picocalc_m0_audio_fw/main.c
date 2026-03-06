/* SPDX-License-Identifier: GPL-2.0 */
/* M0 delta-sigma audio: single TIMER0_CH5 ISR drives both GPIO4_B2 (L) and B3 (R).
 * Power saving: WFE when idle; host must wake M0 via GRF rxev (TRM GRF_SOC_CON37 bit 4).
 * Optional WIC deep sleep: if host sets M0_SHMEM_FLAG_WIC_WAKE and grf_con_mcu_wicenreq
 * (CON37 bit 6), M0 uses WFI+SLEEPDEEP so it can fully power down; host asserts rxev
 * to wake. See RK3506 TRM Part 1 §4.6 (M0 status signals), GRF_SOC_CON37. */

#include "rk3506_regs.h"
#include "shmem.h"

#define REG(addr)   (*(volatile uint32_t *)(addr))

/* Host driver must assert GRF rxev to wake M0: write GRF_SOC_CON37 (GRF_BASE+0x94) with
 * (GRF_CON37_WREN(GRF_CON37_RXEV_BIT) | (1u<<GRF_CON37_RXEV_BIT)), then clear rxev.
 * For WIC deep sleep set CON37 bit 6 (wicenreq) and shmem->flags M0_SHMEM_FLAG_WIC_WAKE. */
#define __WFI()            __asm volatile ("wfi")
#define __WFE()            __asm volatile ("wfe")

/*
 * Fixed config: fewer memory loads in the ISR and no format/sample-rate logic.
 * ALSA on the host can do sample-rate conversion to this rate.
 * Linux driver must use the same sample_rate and buf_size.
 */
#define M0_FIXED_SAMPLE_RATE_HZ  48000U
#define M0_FIXED_BUF_SIZE        8192U
#define M0_FIXED_BUF_MASK         (M0_FIXED_BUF_SIZE - 1U)

/*
 * Single struct for hand-tuned asm ISR: state and constants inlined (one load per
 * access). Only timer_irq, gpio_dr, buf_ptr, shmem_base are addresses; rest are values.
 * Layout byte offsets must match isr.S. Left and right DSM state are contiguous
 * for LDM/STM (I1, I2, LAST, OUT per channel).
 */
typedef struct {
	uint32_t timer_irq;
	uint32_t gpio_dr;
	uint32_t phase_acc;
	uint32_t read_idx;
	uint32_t buf_ptr;
	uint32_t shmem_ctr;
	uint32_t _pad0;
	uint32_t _pad1;
	/* Left channel DSM state (contiguous for LDM/STM) */
	int32_t  integ1_l;
	int32_t  integ2_l;
	int32_t  last_l;
	uint32_t out_l;
	/* Right channel DSM state (contiguous for LDM/STM) */
	int32_t  integ1_r;
	int32_t  integ2_r;
	int32_t  last_r;
	uint32_t out_r;
	uint32_t shmem_base;
	uint32_t rate_48k;
	uint32_t ds_rate;
	uint32_t buf_mask;
	uint32_t dsm_half;
	uint32_t dsm_full;
	uint32_t batch;
	uint32_t gpio_base_mask;
} m0_isr_globs_t;

m0_isr_globs_t m0_isr_globs __attribute__((used)) = {
	.timer_irq     = TIMER0_CH5_BASE + TIMER_INTSTAT,
	.gpio_dr       = GPIO4_BASE + GPIO_DR_L,
	.rate_48k      = M0_FIXED_SAMPLE_RATE_HZ,
	.ds_rate       = DS_RATE_HZ,
	.buf_mask      = M0_FIXED_BUF_MASK,
	.dsm_half      = (uint32_t)DSM_HALF_SCALE,
	.dsm_full      = (uint32_t)DSM_FULL_SCALE,
	.batch         = 8,
	.gpio_base_mask = (0x0C00U << 16),
	.shmem_base    = (uint32_t)M0_SHMEM_ADDR,
};

__attribute__((always_inline)) static inline void gpio_write_both(uint32_t bit_l, uint32_t bit_r)
{
	REG(GPIO4_BASE + GPIO_DR_L) = GPIO4_DR_WRITE(bit_l, bit_r);
}

static void timer5_start(void)
{
	REG(TIMER0_CH5_BASE + TIMER_LOAD0) = DS_PERIOD_TICKS;
	REG(TIMER0_CH5_BASE + TIMER_CTRL) = TIMER_RUN;
}

static void timer5_stop(void)
{
	REG(TIMER0_CH5_BASE + TIMER_CTRL) = TIMER_STOP;
}

__attribute__((always_inline)) static inline void clear_timer5_irq(void)
{
	REG(TIMER0_CH5_BASE + TIMER_INTSTAT) = 1;
}

/* Advance read_idx by step bytes and batch-write to shared memory every 8 frames. */
__attribute__((always_inline)) static inline void advance_read_idx(uint32_t step)
{
	m0_isr_globs.read_idx = (m0_isr_globs.read_idx + step) & m0_isr_globs.buf_mask;
	if (++m0_isr_globs.shmem_ctr >= m0_isr_globs.batch) {
		m0_isr_globs.shmem_ctr = 0;
		((m0_audio_shmem_t *)m0_isr_globs.shmem_base)->read_idx = m0_isr_globs.read_idx;
		__dmb();
	}
}

/* S16 LE stereo only (fixed config). Used from main loop only; ISR uses asm consume. */
__attribute__((always_inline)) static inline void consume_s16_stereo(int32_t *s32_l, int32_t *s32_r)
{
	uint8_t *buf = (uint8_t *)m0_isr_globs.buf_ptr;
	uint32_t i = m0_isr_globs.read_idx & m0_isr_globs.buf_mask;
	*s32_l = (int16_t)(buf[i] | (buf[(i + 1) & m0_isr_globs.buf_mask] << 8));
	i = (m0_isr_globs.read_idx + 2) & m0_isr_globs.buf_mask;
	*s32_r = (int16_t)(buf[i] | (buf[(i + 1) & m0_isr_globs.buf_mask] << 8));
	advance_read_idx(4);
}

/* Hand-tuned asm ISR: load-order scheduling to avoid stalls, minimal push/pop. */
void TIMER0_CH5_IRQHandler(void);

static void nvic_enable_timer5(void)
{
	/* NVIC_ISER0: set bit 19 to enable IRQ 19 */
	REG(0xE000E100) = (1u << TIMER0_CH5_IRQ);
}

static void nvic_disable_timer5(void)
{
	/* NVIC_ICER0: set bit 19 to disable IRQ 19 */
	REG(0xE000E180) = (1u << TIMER0_CH5_IRQ);
}

static void hardware_init(void)
{
	/* CRU: enable TIMER0_CH5 and PCLK_TIMER */
	REG(CRU_BASE + CRU_GATE_CON06) = CRU_TIMER5_EN;
	REG(CRU_BASE + CRU_CLKSEL_CON23) = CRU_TIMER5_100M;
	REG(CRU_BASE + CRU_GATE_CON13) = CRU_GPIO4_EN;

	/* GPIO4 B2/B3: digital mode then output, both low */
	REG(GPIO4_IOC_BASE + SARADC_CON) = SARADC_CON_B23_EN;
	REG(GPIO4_BASE + GPIO_DDR_L) = GPIO4_B23_OUT_DIR;
	gpio_write_both(0, 0);

	/* Timer: load period, don't start yet */
	REG(TIMER0_CH5_BASE + TIMER_LOAD0) = DS_PERIOD_TICKS;
	REG(TIMER0_CH5_BASE + TIMER_CTRL) = TIMER_STOP;
}

/* Re-enable TIMER5 and GPIO4 clocks before starting playback (idle may gate them). */
static void clocks_ungate_play(void)
{
	REG(CRU_BASE + CRU_GATE_CON06) = CRU_TIMER5_EN;
	REG(CRU_BASE + CRU_CLKSEL_CON23) = CRU_TIMER5_100M;
	REG(CRU_BASE + CRU_GATE_CON13) = CRU_GPIO4_EN;
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
		m0_audio_shmem_t *shmem = (m0_audio_shmem_t *)m0_isr_globs.shmem_base;

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
		m0_isr_globs.buf_ptr = (uint32_t)shmem->buffer;
		m0_isr_globs.read_idx = shmem->read_idx;
		m0_isr_globs.shmem_ctr = 0;
		m0_isr_globs.integ1_l = m0_isr_globs.integ2_l = 0;
		m0_isr_globs.integ1_r = m0_isr_globs.integ2_r = 0;
		m0_isr_globs.out_l = m0_isr_globs.out_r = 0;
		m0_isr_globs.phase_acc = 0;
		m0_isr_globs.last_l = m0_isr_globs.last_r = 0;
		__asm volatile("" ::: "memory"); /* compiler barrier: state visible before IRQ */
		nvic_enable_timer5();
		timer5_start();
		while (shmem->ctrl == M0_CTRL_PLAY)
			__WFE();
		timer5_stop();
		REG(TIMER0_CH5_BASE + TIMER_INTSTAT) = 1; /* clear any pending IRQ before masking */
		nvic_disable_timer5();
		gpio_write_both(0, 0);
		clocks_gate_idle();
	}
}
