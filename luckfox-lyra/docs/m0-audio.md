# PicoCalc M0 audio on the Luckfox Lyra (RK3506)

Status: **not yet run on hardware.** Everything below is checked against the
RK3506 TRM Part 1 V1.2, the RK3506G2 datasheet V1.5, the PicoCalc mainboard
V2.0 schematic, and the known-working upstream M0 loader
([nvitya/rk3506-mcu](https://github.com/nvitya/rk3506-mcu)). The firmware's
playback loop is additionally executed in a CPU emulator against a model
(`make check-emu`). What still needs a board is listed under
[Unverified](#unverified-needs-hardware).

## Why this exists

The PicoCalc routes its audio inputs to header pins 31/32. On the Lyra those
are GPIO4_B2/B3 (SARADC_IN2/IN3), which have no PWM function, so the PWM sound
driver needs pins 4/5 bodge-wired across. The RK3506's Cortex-M0 can bit-bang
those two pins fast enough for a 1-bit delta-sigma DAC, which gives sound with
no hardware modification.

```
ALSA (48 kHz S16 stereo) -> picocalc_snd_m0 -> ring in system SRAM
   -> M0 firmware: 2nd-order delta-sigma, one bit per channel per 1 MHz tick
   -> GPIO4_B2 (L, pin 31) / GPIO4_B3 (R, pin 32), 1.8 V
   -> NC7WZ16 buffer (VCC = AXP2101 ALDO4) -> 220R/100R divider + 100 nF -> jack / amp
```

## How the M0 is started

- The Cortex-M0 has no VTOR and fetches its vectors from address 0. The SoC's
  address converter maps M0 address 0 onto the *code start address* passed to
  Rockchip's SiP call `0x82000028`. The firmware is therefore **linked at 0**,
  and `picocalc_rk3506_rproc` loads device addresses `0..0x4000` to
  `0xFFF88000` and passes `0xFFF88000` (the vector table base) as the start
  address. This is how the upstream loader and its test firmware work.
- The core could not run at `0xFFF8xxxx` regardless: on ARMv6-M everything
  from `0xE0000000` up is execute-never. Data accesses there are fine, which
  is how the firmware reaches peripherals and the ring.
- `0xFFF88000`, not `0xFFF84000`: with the stock OP-TEE the latter switches
  the SRAM to TCM mode and locks Linux out of it until reboot, so the firmware
  could not be reloaded (upstream issue #2).

SRAM bank `0xFFF88000..0xFFF8C000`:

| Physical | M0 view | Contents |
|---|---|---|
| `0xFFF88000` (4 KB) | `0x0000` | vectors, code, data, bss, stack |
| `0xFFF89000` (64 B + 8192 B) | same (absolute) | `m0_audio_shmem_t` header and ring |

The ring used to be in DDR at `0x03C00000`. In SRAM the M0 never touches DDR
while playing, and Linux gets its 16 KB back. The `m0_shmem` reserved-memory
node in `picocalc-luckfox-lyra.dtsi` describes it; the driver maps it
write-combined.

## Tick source

TRM facts that decide this (Table 1-5 / Table 4-4, chapters 2, 4, 10):

| Fact | Consequence |
|---|---|
| TIMER0_CH4/CH5 and TIMER1_CH4/CH5 are direct M0 NVIC lines 18-21 | CH5 on line 19 is the right tick; no INTMUX involved |
| Timer channels 0-3 reach the M0 only through the INTMUX (NVIC lines 28-31, masks reset to "all masked") | not used |
| `TIMER_CONTROL` bit 1: 0 = free-running, 1 = user-defined (one shot) | run value is `0x05`, not `0x07` |
| Count-up free-running counts 0..LOAD | LOAD = period - 1 (99 for 1 MHz) |
| `CRU_GATE_CON06` bit 2 `pclk_timer0`, bit 8 `clk_timer0_ch5`; 1 = gated | ungate = `0x01040000` |
| `CRU_CLKSEL_CON23[8:6]` = `clk_timer0_ch5_sel`, `001` = 100 MHz | `0x01C00040`; `stclk_m0_div` starts at bit 9 and is untouched |
| `hclk_m0` is a gate on `aclk_bus_root` (`CRU_CLKSEL_CON21`) | the M0 runs at the bus clock; it cannot be raised alone. The loader logs the rate |
| `GRF_SOC_CON37`: `rxev` bit 3, `sleepholdreqn` bit 4, `wicenreq` bit 5 | header macros corrected (were 4 and 6) |

## Playback loop (`play.S`)

The M0 does nothing else while playing, so there is no interrupt handler.
`m0_play()` owns all registers, keeps both modulators' state in them, and
waits for the timer's `int_pd` status bit.

Per tick: wait, clear, **write the GPIO word computed on the previous tick**,
count down to the next sample, run both modulators. Writing first means every
edge has the same latency from the timer, whatever else that tick does. Two
ticks per sample do extra work, neither of which moves an edge:

- *prefetch* (3 ticks before the swap): check `ctrl` for STOP, read the next
  frame from the ring, scale it, advance and occasionally publish `read_idx`,
  compute how many ticks the next sample lasts (20 or 21 at 1 MHz);
- *swap*: the prefetched frame becomes the modulator input.

Estimated cycles with zero wait states: about 50 for a plain tick, 64 for a
swap tick, 120 for a prefetch tick, against a budget of `hclk_m0 / 1 MHz`
(198 at 198 MHz). An interrupt-driven version of the same work costs about
130 / 180.

Waiting comes in two builds:

- default: poll `int_pd`. Depends on nothing but the timer. The poll loop is
  6 cycles long, so edges land up to 6 cycles late.
- `make TICK_WFI=1`: sleep in WFI. `main.c` sets PRIMASK and enables NVIC
  line 19, so the pending timer interrupt wakes the core without being taken.
  Constant wake latency, and the core idles between ticks.

In simulation (1 kHz tone, 20 Hz-20 kHz band, bit-exact modulator model):

| Case | SNR at -3 dBFS | SNR at -40 dBFS |
|---|---|---|
| ideal edge timing | 49 dB | 17 dB |
| edges late by a random 0-6 cycles of 198 | 43 dB | 6 dB |

So expect roughly 8-9 effective bits at 1 MHz (the ENOB table in
`rk3506_regs.h` is optimistic), and treat polling as the bring-up build: once
WFI wake is confirmed on hardware it should become the default. The random
jitter model is pessimistic for polling, whose delay pattern is periodic, but
where that pattern's energy lands depends on clock ratios not known yet.

## Modulator

Per channel, `FS = 32768`, `G = +FS` when the output bit is 1, else `-FS`:

```
i1 += x - G_prev;  y = i2 + i1;  bit = (y >= 0);  G = bit ? +FS : -FS;  i2 = clamp(y - G)
```

This is a standard second-order modulator with unity signal gain. Three things
matter:

- Both stages use the same full-scale feedback. (The earlier firmware used
  half scale in stage 1, which doubled the gain and overloaded above half
  scale.)
- Input is scaled by 7/8. A 1-bit second-order loop misbehaves close to full
  scale.
- `i2` is clamped to 32x full scale. Scaling alone is not enough: full-scale
  noise or rail-to-rail input at Nyquist otherwise runs the state into int32
  wrap, which is a burst of noise. Ordinary programme peaks at about 11x and
  never reaches the clamp.

`check_dsm.c` mirrors the arithmetic and asserts all of this (`make check`).

## Electrical: ALDO4

| Item | Value | Source |
|---|---|---|
| GPIO4_B2/B3 output high | 1.8 V pad; guaranteed >= 1.4 V, about 1.7 V measured | TRM 17.6.1, datasheet Table 3-3 |
| NC7WZ16 input-high threshold | 0.70 x VCC (VCC 2.3-5.5 V) | onsemi datasheet |
| NC7WZ16 VCC | AXP2101 ALDO4, 0.5-3.5 V in 100 mV steps | schematic |
| Headphone detect | 100 k pull-up to ALDO4, to an STM32 5 V-tolerant input; high = plugged in | schematic, `picocalc_BIOS` |
| STM32 input-high threshold (VDD 3.3 V) | 1.55 V by design; 2.15 V guaranteed | DS5319 Table 36 |
| Other loads on ALDO4 | none | schematic |

| ALDO4 | Buffer needs >= | Margin at 1.7 V | Detect level (after ~0.1 V leakage drop) | Margin over 1.55 V |
|---|---|---|---|---|
| 2.0 V | 1.40 V | 0.30 V | ~1.9 V | 0.35 V |
| **2.1 V** | 1.47 V | 0.23 V | ~2.0 V | 0.45 V |
| 2.2 V | 1.54 V | 0.16 V | ~2.1 V | 0.55 V |
| 2.5 V | 1.75 V | none | ~2.4 V | 0.85 V |

No setting meets both worst-case specifications at once, so this rests on
typical behaviour: start at **2.1 V** and check both the buffer output and
headphone detection on the bench. If detection fails the only symptom is the
speaker amplifier staying on with headphones in.

The STM32 firmware does not program ALDO4 today; it has to. For the AXP2101
that should be register `0x95` = (mV - 500) / 100 with enable bit 3 of `0x90`
(check against the AXP2101 datasheet). The buffer output is divided by
100/320, so the audio swing is about 0.31 x ALDO4: roughly 4 dB quieter at
2.1 V than at 3.3 V. A Pico driving the buffer with 3.3 V is unaffected apart
from that, provided the NC7WZ16 inputs are over-voltage tolerant.

## Host side

- `period_bytes_max` is half the ring. The period-elapsed test compares a
  masked index difference with the period size, so a period as large as the
  ring could never elapse.
- The ring is filled as far as it will go on every timer callback and before
  boot, rather than one period at a time, so it does not run from empty.
- `sync_stop` waits for the worker and the timer before the PCM buffer can be
  freed.
- The M0 is still booted on START and shut down on STOP. Keeping it alive
  between streams (and the WFE/rxev wake that needs) is future work.

## Checks

```
make                 # firmware, arm-none-eabi-gcc
make check           # check_ring (host copy maths), check_dsm (modulator)
make check-emu       # runs m0_play() from the ELF in Unicorn against a model;
                     # needs: pip install unicorn pyelftools
```

`check-emu` compares every GPIO write over 600,000 ticks: silence, DC, sine,
noise across the ring wrap, rail-to-rail input (clamp), underrun hold, an
unaligned start index, clean return on STOP and preserved registers.

## Unverified (needs hardware)

1. The M0 runs at all from `0xFFF88000` with the image linked at 0, and can
   read/write `0xFFF89000` as data. Upstream reports the first; the second
   follows from TRM 7.3.2 but nobody has reported it.
2. `GRF_PMU_MCU_ISO_CON*` (`0xFF911000`..) still allow the M0 to reach GPIO4,
   GPIO4_IOC, CRU, TIMER0 and the SRAM. They reset to "allowed"; the loader or
   OP-TEE could change that. Read them from Linux first.
3. `hclk_m0` rate (logged by the loader at probe) and real cycles per tick.
4. Tick rate is 1.000 MHz (LOAD = 99, and that `clk_gpll_div_100m` really is
   100 MHz on this board).
5. WFI wake with PRIMASK set (`TICK_WFI=1`).
6. ALDO4 window, see above.
7. The device tree's reserved-memory node for a non-DDR address is accepted at
   boot (Rockchip's `rk3506-amp.dtsi` does the same for `0xFFF80000`).

Suggested order: ALDO4 with a static GPIO level; a firmware that just toggles
B2 in a loop (proves 1, 2, gives 3 on a scope); the timer at 1 MHz (4); then
audio.
