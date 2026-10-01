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

/* TIMER0_CH5 — 1 MHz delta-sigma tick.
 * TIMER0_CH4/CH5 (and TIMER1_CH4/CH5) are wired straight to the Cortex-M0
 * NVIC as IRQ 18/19 (20/21); see TRM Table 1-5 / Table 4-4. Channels 0-3 only
 * reach the M0 through the INTMUX (NVIC 28-31), which would cost a mask setup
 * and a flag read per tick, so CH5 is the right tick source. Rockchip's own
 * rk3506-amp.dtsi hands CLK_TIMER0_CH5 to the M0 for the same reason.
 * The playback loop does not take the interrupt: it reads int_pd in
 * TIMER_INTSTAT directly (see play.S), so by default the NVIC is not involved
 * at all; the line only matters for the optional M0_TICK_WFI build. */
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
 * Periodic tick = free-running, count up, interrupt on = 0b0101. */
#define TIMER_RUN         0x05U
#define TIMER_STOP        0x00U

/* CRU */
#define CRU_BASE          0xFF9A0000U
#define CRU_GATE_CON06    0x0818U
#define CRU_GATE_CON13    0x0834U
#define CRU_CLKSEL_CON23  0x035CU
/* CRU registers carry a per-bit write enable in the upper half; in the gate
 * registers a 1 DISABLES the clock, so ungating writes wren bits with data 0.
 * GATE_CON06: bit 2 pclk_timer0, bit 8 clk_timer0_ch5.
 * GATE_CON13: bit 2 pclk_gpio4, bit 3 dbclk_gpio4.
 * CLKSEL_CON23[8:6] clk_timer0_ch5_sel = 0b001 (clk_gpll_div_100m); the
 * neighbouring stclk_m0_div field starts at bit 9 and is not touched. */
#define CRU_TIMER5_EN     0x01040000U
#define CRU_GPIO4_EN      0x000C0000U
#define CRU_TIMER5_100M   0x01C00040U

/* Delta-sigma: 1-bit modulator, 48 kHz sample rate. Effective resolution (ENOB) from
 * oversampling: ENOB ≈ 1 + 2.5*log2(OSR) with OSR = tick rate/48000. 100 MHz timer clock.
 * CPU cycles @200 MHz = 2 * period_ticks (one tick = 10 ns, one CPU cycle = 5 ns).
 *
 *   ENOB (bits) │  OSR   │   tick (Hz)   │ period (ticks) │ cy @200M │ note
 *   ────────────┼────────┼───────────────┼────────────────┼──────────┼────────────────────
 *       14.5    │  42.2  │   2 026 531   │      49        │    98    │ 50 ticks = 2 MHz
 *       14.0    │  36.8  │   1 764 523   │      57        │   114    │
 *       13.5    │  32.0  │   1 536 000   │      65        │   130    │
 *       13.0    │  27.9  │   1 337 269   │      75        │   150    │
 *       12.5    │  24.3  │   1 164 048   │      86        │   172    │
 *       12.0    │  21.1  │   1 013 269   │      99        │   198    │
 *       11.5    │  18.4  │     882 257   │     113        │   226    │
 *       11.0    │  16.0  │     768 000   │     130        │   260    │
 *       10.5    │  13.9  │     668 497   │     150        │   300    │
 *       10.0    │  12.1  │     582 084   │     172        │   344    │
 *        9.5    │  10.6  │     506 876   │     197        │   394    │
 *        9.0    │   9.2  │     441 128   │     227        │   454    │
 *        8.5    │   8.0  │     384 000   │     261        │   522    │
 *        8.0    │   7.0  │     334 061   │     299        │   598    │
 *
 * Current: 100 ticks = 1 MHz tick → ~12 bits (200 cy @200M).
 *
 * The ENOB column is optimistic: it takes OSR against the sample rate rather
 * than twice the audio bandwidth. A bit-exact simulation of this modulator at
 * 1 MHz gives about 49 dB SNR over 20 Hz-20 kHz at -3 dBFS, i.e. 8-9 bits.
 * Each doubling of the tick rate is worth about 15 dB.
 *
 * The M0 core clock is hclk_m0, a plain gate on aclk_bus_root
 * (CRU_CLKSEL_CON21, shared with the rest of the bus domain), so it cannot be
 * raised for the M0 alone. The rproc driver logs the rate at probe; pick the
 * period from this table to fit it. See play.S for the per-tick cycle counts
 * (about 50 for a plain tick, which leaves room to try 50 ticks = 2 MHz once
 * the 1 MHz build is proven on hardware). DS_RATE_HZ must be changed with
 * DS_PERIOD_TICKS: DS_RATE_HZ = 100 MHz / DS_PERIOD_TICKS. */

#define DS_PERIOD_TICKS   100U
/* Count-up free-running counts 0..LOAD inclusive, i.e. LOAD+1 timer clocks per
 * interrupt (same convention as the kernel's rockchip timer driver, which
 * loads freq/HZ - 1). Confirm 1.000 MHz on a scope at bring-up. */
#define DS_TIMER_LOAD     (DS_PERIOD_TICKS - 1U)
#define DS_RATE_HZ        U32(1000000)
/* Modulator: both stages use +/-full-scale (1 << DSM_FS_SHIFT) feedback.
 * The second integrator is clamped to +/-(1 << DSM_CLAMP_SHIFT), 32x full
 * scale: normal programme stays below ~11x, while full-scale noise or
 * Nyquist-rate square waves would otherwise run the state into int32 wrap. */
#define DSM_FS_SHIFT      15
#define DSM_CLAMP_SHIFT   20

/* M0 NVIC line of TIMER0_CH5 (TRM Table 4-4: IRQ 19 = RKTIMER5) */
#define TIMER0_CH5_IRQ    19
#define NVIC_ISER0        U32(0xE000E100)
#define NVIC_ICER0        U32(0xE000E180)
#define NVIC_ICPR0        U32(0xE000E280)

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
