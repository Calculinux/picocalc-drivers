/* SPDX-License-Identifier: GPL-2.0 */
/* RK3506 register definitions for M0 delta-sigma audio (from TRM) */

#ifndef RK3506_REGS_H
#define RK3506_REGS_H

/* Shared with play.S: constants the assembler needs are written with U32()
 * so they lose the C suffix there. */
#ifdef __ASSEMBLER__
#define U32(x)            x
#else
#include <stdint.h>
#define U32(x)            x##U
#endif

/* GPIO4 — bit-bang output (GPIO4_B2 = left, GPIO4_B3 = right) */
#define GPIO4_BASE        U32(0xFF1E0000)
#define GPIO_DR_L          U32(0x0000)
#define GPIO_DDR_L         0x0008U
#define GPIO4_B2_MASK     (1u << (16+10))
#define GPIO4_B3_MASK     (1u << (16+11))
#define GPIO4_B23_OUT_DIR 0x0C000C00U
#define GPIO4_B23_CLR     (GPIO4_B2_MASK | GPIO4_B3_MASK)

/* Build atomic GPIO write: mask bits 26,27; data bits 10,11 from bit_l and bit_r */
#define GPIO4_DR_WRITE(bit_l, bit_r) \
	((0x0C00U << 16) | ((bit_l) << 10) | ((bit_r) << 11))
/* Same word for play.S: write-enable B2/B3 with both low / both high */
#define GPIO4_DR_B23_LOW  U32(0x0C000000)
#define GPIO4_DR_B23_HIGH U32(0x0C000C00)
#define GPIO4_B2_BIT      10
#define GPIO4_B3_BIT      11

/* GPIO4 IOC — GPIO4_B0..B3 are shared with SARADC_IN0..3 and are 1.8 V pads
 * (TRM 17.6.1). GPIO4_IOC_SARADC_CON[7:4] = per-pad GPIO input enable (B0..B3),
 * write-enable in the upper half; 0x00C000C0 sets it for B2/B3 only. */
#define GPIO4_IOC_BASE    0xFF4D8000U
#define SARADC_CON        0x0840U
#define SARADC_CON_B23_EN 0x00C000C0U

/* TIMER0_CH5. The audio firmware does not use it (its tick is SysTick, see
 * below); the diagnostic firmware does.
 * TIMER0_CH4/CH5 (and TIMER1_CH4/CH5) are wired straight to the Cortex-M0
 * NVIC as IRQ 18/19 (20/21); see TRM Table 1-5 / Table 4-4. Channels 0-3 only
 * reach the M0 through the INTMUX (NVIC 28-31). The channel's clock parent
 * has to be selected and enabled by Linux (clocks / assigned-clock-parents
 * in the device tree): "clk_gpll_div_100m" is gated when Linux has no user
 * for it, and is 93.75 MHz rather than 100 on a 1500 MHz GPLL. */
#define TIMER0_CH5_BASE   U32(0xFF255000)
#define TIMER_LOAD0       0x0000U
#define TIMER_LOAD1       0x0004U
#define TIMER_CTRL        0x0010U
#define TIMER_INTSTAT     U32(0x0018)
/* TIMER_6CH_TIMERn_CONTROL (TRM 10.4.3):
 *   bit0 timer_en    1 = enable
 *   bit1 timer_mode  0 = free-running (auto-reload), 1 = user-defined (counts
 *                    to LOAD once and stops until reprogrammed, TRM 10.3.2)
 *   bit2 int_en      1 = interrupt enable
 *   bit3 count_mode  0 = count up
 * Periodic tick = free-running, count up, interrupt on = 0b0101; the period
 * is LOAD + 1 counts (measured). */
#define TIMER_RUN         0x05U
#define TIMER_STOP        0x00U
#define TIMER0_CH5_IRQ    19
#define NVIC_ISER0        U32(0xE000E100)
#define NVIC_ICER0        U32(0xE000E180)
#define NVIC_ICPR0        U32(0xE000E280)

/* CRU registers carry a per-bit write enable in the upper half; in the gate
 * registers a 1 DISABLES the clock, so ungating writes wren bits with data 0.
 * GATE_CON06: bit 2 pclk_timer0, bit 8 clk_timer0_ch5.
 * GATE_CON13: bit 2 pclk_gpio4, bit 3 dbclk_gpio4. */
#define CRU_BASE          0xFF9A0000U
#define CRU_GATE_CON06    0x0818U
#define CRU_GATE_CON13    0x0834U
#define CRU_GPIO4_EN      0x000C0000U

/* SysTick: the audio tick. It is inside the core, so reading and clearing it
 * costs 2 cycles where a TIMER0 access costs about 30, and it wakes WFI with
 * its exception masked, a constant 3 cycles after the reload (all measured
 * with picocalc_m0_diag_fw). It counts the core clock, hclk_m0. */
#define SYST_CSR          U32(0xE000E010)
#define SYST_RVR          U32(0xE000E014)
#define SYST_CVR          U32(0xE000E018)
#define SYST_CSR_RUN      0x7U          /* ENABLE | TICKINT | CLKSOURCE = core clock */
#define SYST_MAX          0x00FFFFFFU
#define SCB_ICSR          U32(0xE000ED04)
#define ICSR_PENDSTSET_BIT 26
#define ICSR_PENDSTCLR_BIT 25

/* Tick timing comes from the host in the shared header (shmem.h): it knows
 * the core clock rate, the firmware does not. These are used only when the
 * header carries none: 187.5 MHz / 312 = 601 kHz.
 *
 * The M0 core clock is hclk_m0, a plain gate on aclk_bus_root
 * (CRU_CLKSEL_CON21, shared with the rest of the bus domain), so it cannot
 * be raised for the M0 alone. Code runs from SRAM over the bus at about 2.5
 * cycles per instruction, and a GPIO write takes 33; see play.S for what a
 * tick costs and build with PROFILE=1 to measure it. */
#define M0_DEFAULT_CORE_HZ      187500000U
#define M0_DEFAULT_TICK_CYCLES  312U

/* Modulator: both stages use +/-full-scale (1 << DSM_FS_SHIFT) feedback.
 * The second integrator is clamped to +/-(1 << DSM_CLAMP_SHIFT), 32x full
 * scale: normal programme stays below ~11x, while full-scale noise or
 * Nyquist-rate square waves would otherwise run the state into int32 wrap.
 * Simulated at a 1 MHz tick this gives about 49 dB SNR over 20 Hz-20 kHz at
 * -3 dBFS (8-9 bits); each doubling of the tick rate is worth about 12 dB. */
#define DSM_FS_SHIFT      15
#define DSM_CLAMP_SHIFT   20

/* GRF — M0 wake/sleep control (TRM §4.6, GRF_SOC_CON37). Host (A7) writes these to
 * wake M0 from WFE or WIC deep sleep. Operational base 0xFF288000.
 * Write-enable: bits 31:16; to write bit N set bit (N+16).
 * Bit 4 is grf_con_mcu_sleepholdreqn (reset 1) — do not confuse with rxev. */
#define GRF_BASE              0xFF288000U
#define GRF_SOC_CON37         0x0094U
#define GRF_CON37_RXEV_BIT    3U   /* grf_con_mcu_rxev: sets the event register, completes WFE */
#define GRF_CON37_WICENREQ_BIT 5U  /* grf_con_mcu_wicenreq: request WIC-based deep sleep */
#define GRF_CON37_WREN(bit)   (1u << (16u + (bit)))

/* Cortex-M0 System Control Block — SLEEPDEEP for WIC deep sleep (full power down). */
#define SCB_BASE              0xE000ED00U
#define SCB_SCR               (SCB_BASE + 0x10U)
#define SCB_SCR_SLEEPDEEP     (1u << 2)

#endif /* RK3506_REGS_H */
