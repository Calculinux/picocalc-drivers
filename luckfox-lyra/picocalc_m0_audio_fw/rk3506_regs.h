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

/* TIMER0_CH5 — 2 MHz delta-sigma interrupt */
#define TIMER0_CH5_BASE   0xFF255000U
#define TIMER_LOAD0       0x0000U
#define TIMER_LOAD1       0x0004U
#define TIMER_CTRL        0x0010U
#define TIMER_INTSTAT     0x0018U
#define TIMER_RUN         0x07U
#define TIMER_STOP        0x00U

/* CRU */
#define CRU_BASE          0xFF9A0000U
#define CRU_GATE_CON06    0x0818U
#define CRU_GATE_CON13    0x0834U
#define CRU_CLKSEL_CON23  0x035CU
#define CRU_TIMER5_EN     0x01040000U
#define CRU_GPIO4_EN      0x000C0000U
#define CRU_TIMER5_100M   0x01C00040U

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

#define TIMER0_CH5_IRQ    19

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
