# PicoCalc M0 audio (firmware + drivers) — pre-bring-up review

Status: **not yet brought up on hardware.** This review cross-checked the code
against the RK3506 TRM (Part 1 V1.2, 2025-08-11), the vendor 6.1 kernel, the
Luckfox U-Boot, Rockchip's own `rk3506-amp.dtsi`, and the public
[nvitya/rk3506-mcu](https://github.com/nvitya/rk3506-mcu) work (issues #1/#2),
including the deep-dive performed in the *"kernel branch comparison survey"*
Claude session (written up in
`calculinux/kernel-update/notes/rk3506-m0-memory.md`, which this document
supersedes for the M0 topic).

Scope:

- `luckfox-lyra/picocalc_m0_audio_fw/` (bare-metal Cortex-M0 firmware)
- `luckfox-lyra/drivers/picocalc_rk3506_rproc/` (remoteproc driver)
- `luckfox-lyra/drivers/picocalc_snd-m0/` (ALSA PCM driver)
- `luckfox-lyra/overlays/m0-audio-overlay.dts`
- `luckfox-lyra/picocalc-luckfox-lyra.dtsi` (`m0_shmem` reservation)

Evidence classes used below:

- **[TRM]** — stated in TRM Part 1 (chapter/section cited).
- **[SIM]** — reproduced by a bit-exact host-side simulator of the ISR maths
  (`picocalc_m0_audio_fw/check_dsm.c`, added with this document, runs under
  `make check_dsm`).
- **[REF]** — from the referenced projects (nvitya/rk3506-mcu issues and
  patch, Rockchip AMP DTS, vendor trees) — credible but only as good as their
  authors' tests.
- **[VERIFY]** — inference that could not be settled from available documents;
  needs an on-hardware measurement.

## Verdict

As written, **the firmware cannot produce sound**. Three independent defects
each fully prevent audio, two of which are provable from the TRM alone:

1. **B1** — `TIMER0_CH5` is not routable to the Cortex-M0 at all, and nothing
   (firmware or host) un-masks the M0 interrupt mux.
2. **B2** — the timer is started in *user-defined (one-shot) count-up mode*;
   even a correctly routed channel would tick exactly once.
3. **B3** — the second-order delta-sigma modulator is *unstable above
   half-scale DC*: the stage-1 integrator winds up without bound (feedback
   ceiling), both integrators wrap `int32`, and the 1-bit output collapses to
   noise. Even below that, the output carries a persistent, level-growing DC
   bias (up to ~6 % at near-full-scale tones).

Two further preconditions must be met externally:

4. **B4** — the speaker input path (NC7WZ16, VCC = AXP2101 ALDO4) only treats
   the 1.8 V M0 output as HIGH when ALDO4 is in a narrow window (~2.2–2.5 V);
   nothing in this branch programs or checks ALDO4.
5. **B5** — the M0 core clock is never set or verified; the 1 µs ISR budget
   requires roughly ≥180–200 MHz, which is an assumption, not a guarantee.

The host-side ring-copy arithmetic is solid (independently re-derived, and
exercised by `check_ring.c`, which passes). Details, severities and fixes are
below; a suggested bring-up order is at the end.

## Findings table

| ID   | Sev      | Where                                   | Issue (short)                                                             |
|------|----------|-----------------------------------------|---------------------------------------------------------------------------|
| B1   | BLOCKER  | `rk3506_regs.h`, `startup.S`, `isr.S`  | TIMER0_CH5 interrupt not connected to M0 INTMUX; mux never un-masked      |
| B2   | BLOCKER  | `rk3506_regs.h` (`TIMER_RUN`)          | CTRL=0x07 = one-shot user-defined mode, not free-running                  |
| B3   | BLOCKER  | `isr.S` (DSM maths)                    | Stage-1 wind-up + s32 wrap above ~half-scale DC; bias at high levels      |
| B4   | PRECOND. | electrical (outside this repo)          | ALDO4 operating window for NC7WZ16 VIH; uncoordinated                     |
| B5   | HIGH     | `rk3506_rproc.c`                        | M0 core clock not set/verified; ISR budget at zero margin                 |
| H1   | MEDIUM   | `rk3506_rproc.c`                        | SiP SMC not pinned to CPU0; no barrier before core release                |
| H2   | MEDIUM   | `m0-audio-overlay.dts`                 | Vendor clock/reset IDs (ABI break on mainline kernel)                     |
| H3   | MEDIUM   | `main.c` idle/WIC path, `shmem.h`      | Dormant WIC path would hang (WFI vs rxev) — and its GRF bits are wrong    |
| H4   | MEDIUM   | `rk3506_regs.h` (GRF macros)           | `GRF_CON37_RXEV_BIT`=4 / `WICENREQ_BIT`=6 — TRM says 3 / 5                |
| M1   | LOW      | `main.c` `hardware_init()`             | RAW CRU writes correct but magic; CON23 write clips `stclk_m0_div[0]`     |
| M2   | LOW      | `main.c` `hardware_init()`             | SARADC unlock: documented subset, deviates from TRM example value         |
| L1   | LOW      | `m0-audio-overlay.dts` comments        | Claim "same pins as PWM audio (RM_IO12/13)" is wrong                       |
| L2   | LOW      | `picocalc_snd_m0.c`                    | Advertised MMAP without `.mmap`; `g_m0` global; misleading SRC comment    |
| L3   | LOW      | various                                | Dead/stub code, duplicate shmem struct (fragile ABI), fmt U8 unused       |

---

## B1 — TIMER0_CH5 is not an M0 interrupt source (BLOCKER)

The Cortex-M0 has 4 external interrupt inputs; they come from a 125→4
round-robin INTMUX at `0xFF2A0000` **[TRM Ch. 4.1, 4.3.1]**. The 125
candidate sources are listed in the TRM's INTMUX connection table (Ch. 1,
Table 1-6). The timer sources present in that table are:

```
76  TIMER0_0      80  TIMER1_0
77  TIMER0_1      81  TIMER1_1
78  TIMER0_2      82  TIMER1_2
79  TIMER0_3      83  TIMER1_3
```

**`TIMER0_4` and `TIMER0_5` do not exist in the M0 INTMUX source list.** They
feed only the Cortex-A GIC, where `TIMER0_5` is SPI **19** **[TRM Ch. 1
A55 interrupt table]**. `#define TIMER0_CH5_IRQ 19` in
`rk3506_regs.h` is therefore almost certainly the *A55 GIC number* recycled
as an M0 NVIC line.

Two consequences, each sufficient by itself:

- The NVIC line the firmware enables (IRQ 19, vector index 35 in
  `startup.S`) corresponds to the top INTMUX bucket (sources 96–125:
  mailbox, DMAC1, MAC MCGR, …) — never the timer **[VERIFY: the
  INTMUX-output → NVIC-line numbering (buckets 0-31/32-63/64-95/96-125 →
  NVIC 16/17/18/19) is the natural reading of the register group
  `INTMUX_IRQ_MASK0..3` but is not spelled out in Part 1 — confirm with a
  flag-register experiment on hardware]**.
- `INTMUX_IRQ_MASK0..3` reset to `0x00000000`, i.e. **all M0 interrupt
  sources are masked at reset** **[TRM Ch. 4.4.2]**, and neither the
  firmware nor the rproc/snd driver ever touches the INTMUX. Even a correctly
  chosen timer channel would never fire.

The overlay's `clocks = <&cru 76>, <&cru 82>` (PCLK_TIMER, CLK_TIMER0_CH5)
match Rockchip's own AMP example holding CH5 **[REF: rk3506-amp.dtsi]** —
that example pairs CH5 with UART/I2C/mailbox usage on the M0, *not* a CH5
interrupt on the M0, which is consistent with the TRM picture above.

### Fix

Pick a timer whose interrupt is muxed to the M0 and configure the mux:

- Channel: `TIMER0_CH0` (base `0xFF250000` **[TRM Ch. 10.4.1]**) is the
  obvious choice: INTMUX source 76, vendor clock ID `CLK_TIMER0_CH0` = 77
  (mainline: renumbered; the session survey mapped HCLK_M0 74→60, STCLK_M0
  104→90, PCLK_TIMER 76→62, CLK_TIMER0_CH5 82→68), gate
  `CRU_GATE_CON06` bit 3, mux field `CRU_CLKSEL_CON22[9:7]` (100 MHz =
  `0b001` — note the `pclk_bus_root` mux/div fields sharing the same
  register) **[TRM Ch. 10, Ch. 2]**.
- Firmware init: `INTMUX_IRQ_MASK2 |= 1 << 12` (`0xFF2A0000 + 0x0008`, source
  76 − 64 = bit 12) **[TRM Ch. 4.4]**.
- Vector: bucket 64–95 → NVIC IRQ 18 (vector index 34) if the numbering above
  is confirmed **[VERIFY]**. Because that bucket is shared with HPTIMER,
  WDT0/1,
  CRYPTO, A7 PMU IRQs, the ISR must identify the source (`INTMUX_IRQ_FLAG2`)
  before clearing the timer and must always clear the timer INTSTAT it raised.
- Overlay: swap `CLK_TIMER0_CH5` (vendor 82) for `CLK_TIMER0_CH0` (vendor 77)
  so the kernel keeps the *actually used* channel clock alive.
  (PCLK_TIMER, 76, stays.)
- Alternative (sidesteps B1 and the shared-bucket discipline entirely): drop
  the timer and tick from a busy-wait delay loop in the M0 main loop. At a
  confirmed ≥180 MHz core clock a 1 µs cycle costs ~180 cycles with no
  interrupt machinery — but it forfeits WFE power savings and makes the
  budget sensitive to core frequency (see B5). The timer route is preferred;
  the mailbox (TRM Ch. 13) is the heavier-weight alternative Rockchip's AMP
  design uses for notifications.

## B2 — Timer is started in one-shot mode (BLOCKER)

`TIMER_RUN = 0x07` in `rk3506_regs.h`. Decoding against the TRM timer
CONTROL register **[TRM Ch. 10.4.3]**:

```
bit0 timer_en     1  enabled
bit1 timer_mode   1  USER-DEFINED count mode (NO auto-reload)
bit2 int_en       1  interrupt enabled
bit3 count_mode   0  count-up
```

In user-defined count-up mode the channel counts 0 → LOAD (`100`), fires
**one** interrupt, and stops: *"it will not automatically reload the count
value register. User need to disable timer firstly and follow the programming
sequence to make timer work again"* **[TRM Ch. 10.3.2]**. Continuous ticking
requires free-running (auto-reload) mode, i.e. `TIMER_RUN = 0x05`.

(Secondary: the TRM programming flow — disable, set mode, load, enable — is
otherwise respected: LOAD is programmed while the timer is stopped in
`hardware_init()` and again in `timer5_start()`.)

Combined with B1, the firmware as written delivers exactly *zero* ticks to
the M0; with only B2 fixed, it delivers *one*.

## B3 — DSM unstable above half-scale DC: wind-up, s32 wrap, bias (BLOCKER)

The 2nd-order modulator in `isr.S`:

```
i1 <- i1 + x + 16384 - (out  << 15)     /* out  in {0,1}, prev channel out  */
y  <- i2 + i1_new
out <- (y >= 0) ? 1 : 0
i2 <- y + 32768 - (out << 16)
```

Static analysis: stage 1's feedback ceiling is `32768·out_avg ≤ 32768`, while
steady state demands `out_avg = (x + 16384)/32768`. For `x ≥ +16384` this is
impossible (`out_avg` would need to exceed 1), so the stage-1 integrator
ramps at `x + 16384 − 32768·out_avg` per consumed sample — at +FS a constant
+16383 (131k samples ≈ 2.7 s to 2³¹), and `i2` accumulates `i1`, so it wraps
far sooner, at roughly √(2·2³¹/16383) ≈ 500 samples ≈ **10 ms** — which is
exactly when the measured duty collapses. −FS winds up the other way at the
same speed.

Dynamic confirmation: the simulator's operations were hand-traced tick-by-
tick against the asm — including the zero-sign convention `asrs #31` of a
zero input — and `check_dsm.c` (1 s runs) ships as a standing regression.
Note the explicit parentheses: in C `<<` binds *looser* than `+`/`-`, so an
unparenthesised port silently left-shifts the whole accumulation every tick.
The table below runs the same bit-exact ops over a 10 s window (10 M DSM
ticks); 440 Hz is rational against 48 kHz, so the duties are true
steady-state values for each stimulus, not transients:

| stimulus                  | expected duty | measured duty | peak \|i1\| | peak \|i2\| |
|---------------------------|---------------|---------------|-------------|-------------|
| DC +16000 (49 % scale)    | 0.988         | 0.988         | 6.0e4       | 1.0e6       |
| DC −16000 (−49 % scale)   | 0.012         | 0.012         | 5.8e4       | 8.9e5       |
| DC +28000 (85 % scale)    | infeasible    | **0.500 (collapsed)** | wrapped (2.1e9) | wrapped (2.1e9) |
| DC ±FS                    | infeasible    | **0.500 (collapsed)** | wrapped     | wrapped     |
| 440 Hz sine, amp 4000     | 0.500         | 0.493         | 2.9e4       | 3.3e4       |
| 440 Hz sine, amp 20000    | 0.500         | 0.465         | 5.9e4       | 1.2e5       |
| 440 Hz sine, amp 32000    | 0.500         | **0.444**     | **4.7e5**   | **4.1e6**   |

Interpretation:

- The modulator is healthy only up to roughly **half scale**: below that, DC
  is tracked exactly and integrators stay at O(full-scale) excursions. Above
  it, stage-1 wind-up is unbounded: `i2` (the quadratic accumulation of the
  winding `i1`) wraps `int32` within ≈ 500 samples ≈ 10 ms, `i1` follows,
  and both channels render as 50 %-duty
  noise. Any audio with DC tilt or sustained levels past ±FS/2 (vocals,
  brass, clippage) corrupts the output.
- Below the stability boundary there is still a **persistent output bias**
  that grows with level (0.7 % at amp 4000 → 5.6 % at amp 32000; ≈ −43 dB →
  −25 dB of reconstruction DC offset): the modulator settles, but not at the
  neutral point.
- Excursions jump ~10× above ~73 % scale (≈4.7e5 / 4.1e6 ≈ 120 × full-scale),
  eroding the margin to wrap for dynamic content — and there is no
  saturation guard anywhere in the ISR.

Fix directions (pick one, then re-validate with the same simulator before
flashing):

1. Replace the topology with a conventional 1-1 cascaded 2nd-order modulator
   (Leven et al.): stage 1 is a *closed* first-order loop
   (`i1 += x − sgn(i1)`), stage 2 integrates `i1` with a matched gain, and
   the output is `sgn(i2)`. Established coefficient sets keep integrator
   excursions at O(full-scale) across the whole input range — no level at
   which wind-up is possible.
2. Cheaper: keep the topology, pre-scale the input (divide by 2 or 4) *and*
   add hard saturation on both integrators (e.g. clamp to ±2²⁸). Trades
   dynamic range (~12 dB) for bounded, predictable behaviour; still verify
   in sim (the DC stability envelope moves with the scale factor).

Whichever route: add `check_dsm`-style regression coverage for DC, square
and full-scale sine, with an assertion that neither integrator approaches
2³⁰.

## B4 — Electrical precondition: ALDO4 operating window (PRECONDITION)

Hardware path (per PicoCalc design): M0 GPIO4_B2_z/B3_z (header pins 32/31,
1.8 V `_z` domain) → **NC7WZ16** (dual buffer, *non-inverting* → 1-bit
polarity is preserved) with VCC fed from AXP2101 **ALDO4** (adjustable
0.5–3.5 V, set by the STM32) → amplifier/speaker.

- NC7WZ16: `VIH = 0.70·VCC`. For the M0's 1.8 V HIGH to be recognized with
  margin, ALDO4 must be ≲ 2.4–2.5 V (hard ceiling 1.8 V / 0.7 ≈ 2.57 V).
- The same ALDO4 rail drives the headphone-detect pull-up whose HIGH level
  the STM32 must still qualify — that sets the lower bound.
- Net: a narrow workable band, roughly **2.2–2.5 V**, to be nailed down on
  the bench.

Nothing in this branch (driver, overlay, firmware) programs ALDO4 or gates
playback on it — playback can therefore be electrically dead out of the
box. Actions: measure the band (buffer output swing + headphone-detect
behaviour) and document the required ALDO4 setpoint; consider exposing it so
the audio bring-up can assert it before `START`.

(For contrast: the *modified* PWM path outputs the waveform on
RM_IO12/13 = GPIO0_B4/B5, which is why that path needs the 4/5→32/31
bodge; the M0 path drives 32/31 natively. See L1 — the overlay comment
conflates the two.)

## B5 — M0 core clock never set or verified (HIGH)

The ISR budget assumes ~200 MHz: the ENOB table in `rk3506_regs.h` and the
cycle annotations in `isr.S` ("one tick = 10 ns, one CPU cycle = 5 ns").
A worst-case ISR tick (consume + batch store + both channels + GPIO +
exception entry) is on the order of 160–200 cycles, versus a budget of
187.5–200 cycles per 1 µs — **zero margin** even if the core really runs at
187.5–200 MHz, and the M0 is fetching from AXI-mode system SRAM (no TCM,
extra wait states; see the memory-mode notes below).

The rproc driver only `clk_bulk_prepare_enable`s `HCLK_M0`/`STCLK_M0`
(AHB/standby clocks) and never programs any M0 core rate; Part 1 of the TRM
does not even expose the M0 core PLL (the M0 core clock appears to be
secure-domain configuration). The reference implementation reportedly locks
the core at 187.5 MHz deliberately (**[REF**: `"M0 core clock locked at
187500000Hz"` in nvitya/rk3506-mcu issue #2 log) — i.e. this is something
you have to *do*, not something you inherit.

Actions: measure the on-target M0 core frequency (count a known GPIO-toggle
loop with a scope/analyzer, or differential timing from the A55 side); if it
is < ~180 MHz, either adopt the reference driver's lock sequence in
`rk3506_rproc_start()` or relax `DS_PERIOD_TICKS` (the author's own table
shows the ENOS trade-off, e.g. 113 ticks → ~11.5 ENOB).

## H1 — rproc driver: SiP call on CPU0, barrier before release (MEDIUM)

- `rk3506_rproc_start()` issues `arm_smccc_smc(0x82000028, ...)` (Rockchip
  MCU_CFG, `CODE_START_ADDR` sub-op) from whichever CPU the workqueue happens
  to run on. The patched reference driver pins this to CPU0: Rockchip's
  OP-TEE maps the secure GRF on the boot CPU, and the call from another
  core can trip its VA/PA consistency check **[REF: patch in
  nvitya/rk3506-mcu#2]**. Cheap insurance: wrap in `work_on_cpu(0, ...)`.
  **[VERIFY on hardware]**
- Add a `wmb()`/`dsb` between finishing the firmware load and releasing the
  core (reset de-assert / SiP start) **[REF: same patch]**.
- Reset handling: `SRST_H_M0`/`m0_jtag`/`hresetn_m0_ac` are de-asserted once
  at probe and never re-asserted. The stop path's PMU write is nonetheless a
  real core reset — see the verified-values note below — so this is accepted
  as-is, with a fallback: if bring-up ever observes the M0 surviving
  `rproc_shutdown`, re-assert `SRST_H_M0` in `ops->stop`.

### Verified: the PMU magic values decode correctly

`writel(0x00060004/0x00060002, PMU+0x00C)` → `PMU_INT_MASK_CON` **[TRM
Ch. 6]**: bits 31:16 are per-bit write enables; bit 2 =
`mcu_rst_dis_cfg` (0 = assert MCU reset, 1 = release); bit 1 =
`glb_int_mask_mcu`.

- run  = wren{1,2} + release reset + unmask M0 ints ✓
- stop = wren{1,2} + assert reset  + mask  M0 ints ✓

(The reference patch stops with reset held *released*; ours asserting it is
at least as safe for reload. No change needed.)

## H2 — Overlay hard-codes vendor 6.1 clock/reset IDs (MEDIUM)

`m0-audio-overlay.dts` uses `clocks = <&cru 74>, <&cru 104>, <&cru 76>,
<&cru 82>` and `resets = <&cru 90>, <&cru 91>, <&cru 10>` — vendor 6.1
numbering. The session survey established the mainline renames: HCLK_M0
74→60, STCLK_M0 104→90, PCLK_TIMER 76→62, CLK_TIMER0_CH5 82→68, SRST_H_M0
90→35, SRST_M0_JTAG 91→36, SRST_HRESETN_M0_AC 10 → SRST_H_M0_AC 8. Today's
Calculinux kernel is vendor 6.1, so the overlay is *correct for now*; it
becomes an ABI break the moment the M0 work lands on the mainline kernel.
Keep a small mapping table in the overlay comment (and fold the B1 channel
swap in there).

## H3/H4 — Dormant WIC/deep-sleep path: would hang, and cites wrong GRF bits (MEDIUM)

`main.c`'s WIC branch (entered when `M0_SHMEM_FLAG_WIC_WAKE` is set)
programs `SLEEPDEEP` then polls `shmem->ctrl` in `WFI`. Two problems:

- `WFI` only wakes on a *pending interrupt* (or WIC event). A host write to
  the shared-memory `ctrl` field generates no wake — the GRF `rxev` line sets
  the *event register*, which unblocks **WFE**, not WFI **[TRM Ch. 5,
  GRF_SOC_CON37]**. As written, the M0 would sleep forever and audio could
  only be resumed by an actual M0-visible interrupt (mailbox/WIC), which the
  driver does not set up. The snd driver's comment already concedes this path
  is unreachable (boot/shutdown every play) — good; the risk is someone
  "enabling" it by just setting the flag.
- The GRF bit numbers in `rk3506_regs.h` are off by one: TRM
  `GRF_SOC_CON37` (base `0xFF288000` + `0x0094`) **[TRM Ch. 5]** has
  `grf_con_mcu_rxev` = **bit 3** (firmware: 4, and 4 is actually
  `grf_con_mcu_sleepholdreqn`), `grf_con_mcu_wicenreq` = **bit 5** (firmware:
  6, reserved). Fix the macros (and the `main.c` comment) before any future
  keep-M0-alive phase; the WREN macro (bit N+16) is correct.

Until then: keep the flag path guarded in comments as "experimental, known
broken" rather than "optional".

## M1 — Raw CRU writes: correct, but magic (LOW)

Decoded against the TRM (all three writes use the CRU's per-bit write-enable
shadow, so they are surgical — verified):

| write                          | effect                                                                       |
|--------------------------------|------------------------------------------------------------------------------|
| `GATE_CON06 = 0x01040000`      | ungates `pclk_timer0` + `clk_timer0_ch5` (bits 2, 8 → 0)                     |
| `GATE_CON13 = 0x000C0000`      | ungates `pclk_gpio4` + `dbclk_gpio4` (bits 2, 3 → 0)                         |
| `CLKSEL_CON23 = 0x01C00040`    | `clk_timer0_ch5_sel = 0b001` (clk_gpll_div_100m); *side effect*: forces `stclk_m0_div[0] ← 0` (no-op iff the post-boot divider is even, which the value's provenance suggests) |

Recommendations: replace the CH5 values with the B1 fix (CON22/CH0) and
prefer the kernel clk framework for the mux (the rproc node already owns the
clock); if the raw writes stay, add TRM-derived register/bit comments instead
of anonymous hex.

## M2 — SARADC-shared pad unlock (LOW)

GPIO4_B0–B3 are shared with SARADC. TRM §17.6.1: *before using them as common
GPIO, send `0x00F000F0` to `GPIO4_IOC_SARADC_CON` (`0xFF4D8840`)*. The
firmware writes `0x00C000C0` instead: decoded against the register fields
(bits 7:4 = `gpio_saradc_ie` per pad B0–B3, wren in the upper half) this
sets the receiver enable for **exactly B2 and B3** — a tighter, defensible
subset of the TRM's example, with the A55-side IOMUX selection handled by the
overlay pinctrl. Acceptable; annotate the value with the TRM citation and
note the deliberately narrowed scope. Default pad config (ds=Level1, slew on,
no pull) is adequate for a 1-bit output into a CMOS buffer input.

## L1 — Overlay comment misstates the pinout (LOW)

`m0-audio-overlay.dts` says *"Uses the same pins as PWM audio (RM_IO12/13 as
GPIO for bit-banged output)"*. That conflates two different paths:

- RM_IO12/13 are matrix-IO pads = **GPIO0_B4/B5** `[vendor
  rk3506-pinctrl-rmio.dtsi]` — the PWM-driver waveform pins, requiring the
  4/5→32/31 bodge wire.
- The M0 overlay correctly selects **GPIO4_PB2/PB3** = header 32/31
  (`SARADC_IN2/3`), the natively wired speaker inputs.

Rewrite the comment to describe the native path and to warn only that the
PWM overlay must not be active at the same time (the `leftpin/rightpin`
gpios it uses are the same B2/B3).

## L2 — ALSA driver cosmetics (LOW)

- `hw.info` advertises `SNDRV_PCM_INFO_MMAP | _MMAP_VALID` but there is no
  `.mmap` op — drop the flags (or implement mmap later).
- `g_m0` global singleton: not needed; `card->private_data` already carries
  the pointer.
- Comment claims "ALSA does SRC to this rate" while the constraint set hard-
  fixes 48 kHz (`SNDRV_PCM_RATE_48000`) — that's deliberate and fine, but the
  SRC happens in userspace plugins, not "ALSA"; reword to avoid confusion.
- The period-elapsed heuristic `((Δ read_idx) & mask) >= period_bytes` is
  only alias-free while per-callback consumption stays below the ring size
  (true under the current constraints: max drain 8188 B < 8192 B). Safe, but
  razor-thin — add a comment stating the invariant.

## L3 — Housekeeping (LOW)

- `clocks_gate_idle()` is an empty stub with a good explanation — keep, but
  mark it `/* TODO(kill or implement with keep-alive M0 model) */`.
- `M0_FMT_U8` is unused (firmware is S16-only); delete or implement.
- `m0_audio_shmem_t` is duplicated (firmware `shmem.h` vs. `struct
  m0_audio_shmem` in `picocalc_snd_m0.c`) with no compile-time link — a
  shared header (or a `BUILD_BUG_ON` on offsets) would prevent silent ABI
  drift.
- `consume_s16_stereo()` in `main.c` is dead code (ISR consumes in asm); it
  is gc-sectioned out, delete it anyway to avoid it becoming a misleading
  "reference implementation".

---

## What checked out OK (vetted, no action)

- **Vector table**: `startup.S` layout matches the Cortex-M0 table (SysTick
  15, external IRQs from 16; the TIMER0_CH5 handler sits at vector index 35
  = IRQ 19 — the *placement* is consistent; what is wrong is the routing,
  see B1).
- **Timer register map**: base `0xFF255000` (CH5), stride `0x1000`,
  `LOAD 0x0000 / CONTROL 0x0010 / INTSTATUS 0x0018` — matches TRM Ch. 10.4.
- **GPIO4**: base `0xFF1E0000`, `DR_L 0x00 / DDR_L 0x08`; DR/DDR bits 10/11 =
  PB2/PB3 under the 8-pin-subgroup layout; the stray `0x0C000000` upper bits
  address unused pins and are ignored (TRM §17.6). NVIC ISER/ICER encodings
  correct.
- **SRAM landing site**: linking at `0xFFF88000` (SRAM2, 16 KiB) with the ELF
  entry passed as `CODE_START_ADDR` is the *reload-safe* OP-TEE mode — any
  start address other than `0xFFF84000` leaves the SRAM AXI-accessible, so
  the M0 can be re-flashed on every play **[REF: nvitya/rk3506-mcu#2, TRM
  Ch. 7.3]**. Cost acknowledged: no TCM (AXI fetch latencies — relevant to
  B5's budget). `0xFFF81000` and below stay off-limits (OP-TEE).
- **Stack/layout**: `_stack_top = 0xFFF8C000` = end of SRAM2; vectors first,
  `.ramfunc` ISR immediately after; resource table present with `num = 0` —
  satisfies the kernel ELF loader's sanity check.
- **Shared ring**: `0x03C00000` (16 KiB, `no-map`) is DDR; Rockchip's own
  `rk3506-amp.dtsi` parks M0-visible rpmsg memory at exactly `0x3C00000`, so
  M0→DDR reachability on the rkbin-OP-TEE boot chain is established practice
  **[REF]**. Caveats: untested on this exact tree, and if the M0 ever reads
  garbage from the ring the first suspects are the `GRF_PMU_MCU_ISO_*`
  registers (`0xFF911000`+); also do **not** coexist with Rockchip's amp
  dtsi reservation (`0x3C00000`, 2 MiB) — overlapping.
- **Resampler**: phase accumulator `48000` @ 1 MHz vs `DS_RATE 1 000 000` —
  exact average (20.833 ticks/sample), worst-case ±1 tick (±1 µs) skew per
  sample ≪ the 21 µs frame period. Inaudible.
- **Host copy math**: ring space/full-empty sentinel (1 wasted slot,
  capacity 8191 B ≈ 42 ms), boundary-wrapped `appl/copied` accounting, and
  frame-aligned copy sizing — re-derived by hand, exercised by
  `check_ring.c` (**passes**), and consistent with the 8-sample (32 B)
  conservative lag of the M0's batched `read_idx` publication.

## Suggested bring-up order

Cheapest-highest-information-first; each step isolates one layer so a
failure is attributable:

1. **Electrical precondition (B4)** — power the board, set ALDO4 into the
   2.2–2.5 V window, confirm the NC7WZ16 passes a 1.8 V HIGH from a known
   source (e.g. a GPIO via the same buffer) and that the headphone-detect
   pull-up still reads HIGH on the STM32. No M0 involved.
2. **M0 runs at all** — flash a minimal "spin forever / blink a third GPIO"
   ELF through the existing rproc path. This exercises the SiP call, TCM/AXI
   mode, reset de-assert and core bring-up independently of the audio code.
   *(This is where H1's CPU0/barrier concern surfaces first.)*
3. **Core clock (B5)** — with the blink ELF, measure the actual M0 core
   frequency (scope the blink or count against an A55-side hrtimer delta).
   Record it; it decides whether the 1 µs tick period is viable (step 4).
4. **Timer ticks reach the M0 (B1)** — fix the channel to TIMER0_CH0, un-mask
   INTMUX source 76, and have the ISR toggle a spare GPIO. Scope the GPIO:
   you want a 1 MHz square wave. This confirms INTMUX routing, NVIC line
   numbering **[VERIFY]** and the free-running control value (B2: 0x05).
5. **Drive the real pads (M1/M2)** — route the ISR's bit out to GPIO4_B2/B3
   (through the NC7WZ16). Confirm with a scope that the pad actually
   transitions (validates the SARADC unlock, IOMUX and DR bit positions).
6. **Modulator correctness (B3)** — with a step/DC generator into the ring,
   watch duty follow level *and* keep integrators bounded (run
   `check_dsm` in host first; it is the gate for shipping a new `isr.S`).
7. **End-to-end audio** — play a known tone through ALSA, listen/scope the
   speaker, then musical content and the full B4 window.

Only after 1–4 pass is it worth debugging 5–7; each blocker above hides the
next, so the order matters.

## Review logistics

This review is delivered as a documentation PR against `main` (the
`m0-audio` branch it was cut from was merged via PR #25 and deleted upstream)
with no functional changes. The substantive fixes will follow in separate,
sequenced PRs so each is independently reviewable and testable:

- **Round 2 (firmware, B1+B2):** switch to TIMER0_CH0, INTMUX un-mask,
  free-running control word, ISR source identification.
- **Round 3 (firmware, B3):** modulator redesign + saturation guards, gated
  by `check_dsm`.
- **Round 4 (drivers, H1–H4):** CPU0 SMC, barriers, mainline clock/reset IDs,
  GRF bit corrections.
- **Round 5 (electrical, B4):** ALDO4 setpoint + coordination, folded into
  whatever owns the AXP2101 regulator config.

The M1/M2/L1/L2/L3 cleanups ride along in whichever round touches their file.