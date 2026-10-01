/* SPDX-License-Identifier: GPL-2.0 */
/* RK3506 register definitions for M0 delta-sigma audio (from TRM) */

#ifndef RK3506_REGS_H
#define RK3506_REGS_H

#include <stdint.h>

/* GPIO4 — bit-bang output (GPIO4_B2 = left, GPIO4_B3 = right) */
#define GPIO4_BASE        0xFF1E0000U
#define GPIO_DR_L          0x0000U
#define GPIO_DDR_L         0x0008U
#define GPIO4_B2_MASK     (1u << (16+10))
#define GPIO4_B3_MASK     (1u << (16+11))
#define GPIO4_B23_OUT_DIR 0x0C000C00U
#define GPIO4_B23_CLR     (GPIO4_B2_MASK | GPIO4_B3_MASK)

/* Build atomic GPIO write: mask bits 26,27; data bits 10,11 from bit_l and bit_r */
#define GPIO4_DR_WRITE(bit_l, bit_r) \
	((0x0C00U << 16) | ((bit_l) << 10) | ((bit_r) << 11))

/* GPIO4 IOC — enable digital mode for B2/B3 */
#define GPIO4_IOC_BASE    0xFF4D8000U
#define SARADC_CON        0x0840U
#define SARADC_CON_B23_EN 0x00C000C0U

/* TIMER0_CH0 — 1 MHz delta-sigma tick source.
 * CH0 was chosen because its interrupt is one of the 125 INTMUX sources that
 * can actually reach the Cortex-M0 (INTMUX source 76, TRM Table 1-6); the
 * former CH5 (and all CH4/CH5 of both timers) only feed the A55 GIC and can
 * never interrupt the M0. TRM Ch. 10.4.1: TIMER0_CH0 = 0xFF250000, channel
 * stride 0x1000, offsets per Table 10.4.2. */
#define TIMER0_CH0_BASE   0xFF250000U
#define TIMER_LOAD0       0x0000U
#define TIMER_LOAD1       0x0004U
#define TIMER_CTRL        0x0010U
#define TIMER_INTSTAT     0x0018U
/* CONTROL bit layout (TRM Ch. 10.4.3):
 *   bit0 timer_en        1 = enable
 *   bit1 timer_mode      0 = free-running (auto-reload), 1 = user-defined
 *                        (counts 0->LOAD once and STOPS — not usable for a
 *                        periodic tick)
 *   bit2 int_en          1 = raise interrupt at expiry
 *   bit3 count_mode      0 = count up
 * Free-running count-up with interrupt = 0b0101. The previous value 0x07
 * was one-shot mode: at most one tick per boot. */
#define TIMER_RUN         0x05U
#define TIMER_STOP        0x00U

/* M0 INTMUX (TRM Ch. 4.3.1, 4.4.2): 125 peripheral sources arbitrated onto
 * 4 lines that drive the M0 NVIC. Mask registers are per-32-source groups,
 * bit value 1 = UNMASK, all bits reset masked. FLAG registers are RO and
 * clear when the underlying source is serviced (here: timer INTSTAT write).
 *
 * TIMER0_CH0 is source 76 (TRM Table 1-6) -> INTMUX_IRQ_MASK2/FLAG2 bit 12
 * (the register group covers sources 64-95).
 *
 * Which of the four NVIC lines (IRQ 16-19) carries INTMUX output 2 is not
 * stated in TRM Part 1. The firmware therefore enables all four lines and
 * installs the handler at all four vector slots; the ISR discriminates via
 * FLAG2 bit 12 before touching the timer. Once the mapping is measured on
 * hardware (bring-up step 4 of docs/m0-audio-review.md), collapse
 * M0_EXT_IRQ_LINES / the vector table to the single active line. */
#define INTMUX_BASE           0xFF2A0000U
#define INTMUX_IRQ_MASK2      (INTMUX_BASE + 0x0008U)
#define INTMUX_IRQ_FLAG2      (INTMUX_BASE + 0x0088U)
#define INTMUX_SRC_TIMER0_CH0 (1u << (76u - 64u)) /* MASK2/FLAG2 bit 12 */
#define M0_EXT_IRQ_LINES      0x000F0000U /* NVIC IRQ 16..19, hedge (above) */

/* CRU */
#define CRU_BASE          0xFF9A0000U
#define CRU_GATE_CON06    0x0818U
#define CRU_GATE_CON13    0x0834U
#define CRU_CLKSEL_CON22  0x0358U
/* CRU writes use the per-bit write-enable shadow: upper half selects which
 * lower-half bits are written. 1 = clock DISABLED (gate asserted).
 * GATE_CON06: ungate pclk_timer0 (bit 2) + clk_timer0_ch0 (bit 3),
 * TRM CRU_GATE_CON06. Only those two bits are written — the rest of the
 * register (wdt/wdt1/mailbox/intmux/spinlock gates, ch1-ch5) is untouched. */
#define CRU_TIMER0_CH0_EN 0x000C000CU
/* GATE_CON13: ungate pclk_gpio4 + dbclk_gpio4 (bits 2, 3). */
#define CRU_GPIO4_EN      0x000C0000U
/* CLKSSEL_CON22: clk_timer0_ch0_sel[9:7] = 0b001 (clk_gpll_div_100m,
 * 100 MHz), wren exactly those bits. Deliberately avoids the old CH5
 * write to CON23, which also clipped stclk_m0_div[0]. Bits 6:0 of CON22
 * (pclk_bus_root sel/div) are untouched. */
#define CRU_TIMER0_CH0_100M 0x01C001C0U

/* Delta-sigma: 1-bit modulator, 48 kHz sample rate. Effective resolution (ENOB) from
 * oversampling: ENOB ≈ 1 + 2.5*log2(OSR) with OSR = ISR/48000. 100 MHz timer clock.
 * CPU cycles @200 MHz = 2 * period_ticks (one tick = 10 ns, one CPU cycle = 5 ns).
 *
 *   ENOB (bits) │  OSR   │   ISR (Hz)    │ period (ticks) │ cy @200M │ note
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
 * Current: 100 ticks = 1 MHz ISR → ~12 bits (200 cy @200M).*/

#define DS_PERIOD_TICKS   100U
#define DS_RATE_HZ        1000000U
#define DSM_FULL_SCALE    32768
#define DSM_HALF_SCALE    16384

/* Data memory barrier — ensures M0 store is visible to A55 before proceeding */
#define __dmb()  __asm volatile("dmb" ::: "memory")

/* GRF — M0 wake/sleep control (TRM §4.6, GRF_SOC_CON37). Host (A55) writes these to
 * wake M0 from WFE or WIC deep sleep. Operational base 0xFF288000.
 * Write-enable: bits 31:16; to write bit N set bit (N+16). */
#define GRF_BASE              0xFF288000U
#define GRF_SOC_CON37         0x0094U
#define GRF_CON37_RXEV_BIT    4U   /* Assert to wake M0 from WFE (or WIC wake) */
#define GRF_CON37_WICENREQ_BIT 6U  /* Host sets 1 for WIC-based deep sleep (M0 can power down) */
#define GRF_CON37_WREN(bit)   (1u << (16u + (bit)))

/* Cortex-M0 System Control Block — SLEEPDEEP for WIC deep sleep (full power down). */
#define SCB_BASE              0xE000ED00U
#define SCB_SCR               (SCB_BASE + 0x10U)
#define SCB_SCR_SLEEPDEEP     (1u << 2)

#endif /* RK3506_REGS_H */
