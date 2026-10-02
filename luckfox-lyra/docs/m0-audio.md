# PicoCalc M0 audio on the Luckfox Lyra (RK3506)

Status: **plays on hardware** (Luckfox Lyra RK3506G2 in a PicoCalc,
Calculinux 6.1.99, 2026-10-01): test tones through the ALSA device, at a
600 kHz output bit rate. Audio quality has not been measured or judged yet,
and two timing issues are open (see [Open issues](#open-issues)).

Sources: RK3506 TRM Part 1 V1.2, RK3506G2 datasheet V1.5, the PicoCalc
mainboard V2.0 schematic, the upstream M0 loader
([nvitya/rk3506-mcu](https://github.com/nvitya/rk3506-mcu)), and measurements
taken on the board with [`picocalc_m0_diag_fw`](../picocalc_m0_diag_fw/README.md).

## Why this exists

The PicoCalc routes its audio inputs to header pins 31/32. On the Lyra those
are GPIO4_B2/B3 (SARADC_IN2/IN3), which have no PWM function, so the PWM sound
driver needs pins 4/5 bodge-wired across. The RK3506's Cortex-M0 can bit-bang
those two pins fast enough for a 1-bit delta-sigma DAC, which gives sound with
no hardware modification.

```
ALSA (48 kHz S16 stereo) -> picocalc_snd_m0 -> ring in system SRAM
   -> M0 firmware: 2nd-order delta-sigma, one bit per channel per tick
   -> GPIO4_B2 (L, pin 31) / GPIO4_B3 (R, pin 32), 1.8 V
   -> NC7WZ16 buffer (VCC = AXP2101 ALDO4) -> 220R/100R divider + 100 nF -> jack / amp
```

## How the M0 is started

- The Cortex-M0 has no VTOR and fetches its vectors from address 0. The SoC's
  address converter maps M0 address 0 onto the *code start address* passed to
  Rockchip's SiP call `0x82000028`. The firmware is therefore **linked at 0**,
  and `picocalc_rk3506_rproc` loads device addresses `0..0x4000` to
  `0xFFF88000` and passes `0xFFF88000` (the vector table base) as the start
  address. (On ARMv6-M the core could not execute at `0xFFF8xxxx` anyway:
  everything from `0xE0000000` up is execute-never.)
- `0xFFF88000`, not `0xFFF84000`: with the stock OP-TEE the latter switches
  the SRAM to TCM mode and locks Linux out of it until reboot, so the firmware
  could not be reloaded (upstream issue #2). The price is speed, see below.
- The loader does not boot the M0 at probe; `picocalc_snd_m0` boots it when a
  stream starts and shuts it down when it stops. Stop and reload work.

SRAM bank `0xFFF88000..0xFFF8C000`:

| Physical | M0 view | Contents |
|---|---|---|
| `0xFFF88000` (4 KB) | `0x0000` | vectors, code, data, bss, stack |
| `0xFFF89000` (64 B + 8192 B) | same (absolute) | `m0_audio_shmem_t` header and ring |

The ring is described by a plain node in the `m0-audio` overlay; nothing is
reserved in the base device tree. **Keep it in SRAM**: with the ring in DDR
(`0x03C00000`) about 30 % of all ticks overran at 600 kHz, against a few
hundred per second-long run with the ring in SRAM. M0 accesses to DDR are slow.

## What things cost on this SoC

Measured at `hclk_m0` = 187.5 MHz (GPLL 1500 MHz / 8), image running from SRAM
over the bus, not as TCM:

| | Cycles | With no wait states |
|---|---|---|
| straight-line instruction | 2.5 | 1 |
| two-instruction loop with a taken branch | 10 | 4 |
| GPIO4 DR write, start to finish | 33, constant | 2 |
| TIMER0 register read, start to finish | 30, constant | 2 |
| core-internal register (NVIC, SysTick) | 2 | 2 |
| SRAM read at `0xFFF89000` / through the address-0 window | 6 / 2 | 2 |

`hclk_m0` is a gate on `aclk_bus_root` (`CRU_CLKSEL_CON21`): the M0 runs at
the bus clock and cannot be raised on its own. `clk_gpll_div_100m` is
93.75 MHz here, not 100, and Linux gates it when it has no user.

## Tick: SysTick and WFI

The firmware takes no interrupts. PRIMASK stays set; SysTick's exception is
enabled, so when it becomes pending it wakes the core from WFI and is never
taken. Measured:

- wake is a constant 3 cycles after the SysTick reload;
- consecutive timer-woken wakes are exactly the same number of cycles apart
  (zero spread over 4096);
- a GPIO write takes a constant 33 cycles.

So every output edge lands the same number of core cycles after its tick, and
a tick costs no peripheral access apart from that one GPIO write. Polling a
TIMER0 status bit instead costs 30 cycles per look and jitters by about
280 ns; TIMER0 also needs its clock parent selected and enabled by Linux.
TIMER0_CH5 is a direct M0 NVIC line (19) and does wake WFI, but SysTick is
cheaper and exact.

The driver tells the firmware how many core cycles a tick is and how many
ticks a sample lasts (whole part plus a remainder spread Bresenham-style),
from `clk_get_rate()` of `hclk_m0` and its `tick_hz` parameter. The firmware
has no clock rate built in beyond a fallback.

## Playback loop (`play.S`)

`m0_play()` owns all registers and keeps both modulators' state in them. Per
tick: WFI, **write the GPIO word computed on the previous tick**, clear the
pending bit, count down to the next sample, run both modulators. The last
four ticks of each sample share out the per-sample work so that no tick
carries all of it: check `ctrl` and work out the next sample's length; read
the next frame from the ring; advance and publish `read_idx`; swap the frame in.

Measured with a `PROFILE=1` build (the loop records the longest tick and
counts overruns; the driver prints them when playback stops):

| `tick_hz` | Cycles per tick | Ticks that overran, of all ticks |
|---|---|---|
| 600 kHz | 313 | about 0.01 % (the stalls under Open issues) |
| 625 kHz | 300 | the same |
| 650 kHz | 288 | 7 % (one per-sample tick no longer fits) |
| 700 kHz | 268 | 19 % |
| 750 kHz | 250 | 32 % |

The heaviest tick is about 295 cycles, so **600 kHz is the default**. An
overrun delays that tick's edge and the next tick starts late; ticks are not
lost unless the work exceeds two ticks.

## Modulator

Per channel, `FS = 32768`, `G = +FS` when the output bit is 1, else `-FS`:

```
i1 += x - G_prev;  y = i2 + i1;  bit = (y >= 0);  G = bit ? +FS : -FS;  i2 = clamp(y - G)
```

A standard second-order modulator with unity signal gain. Both stages use the
same full-scale feedback; input is scaled by 7/8; `i2` is clamped to 32x full
scale, because scaling alone does not stop full-scale noise from running the
state into int32 wrap. `check_dsm.c` mirrors the arithmetic and asserts this.

Simulated in-band SNR (1 kHz tone, 20 Hz-20 kHz, ideal edges) is about 49 dB
at -3 dBFS for a 1 MHz tick, and falls about 12 dB per halving of the tick
rate: expect roughly 40 dB at 600 kHz. That is a simulation of the modulator
alone, not a measurement of this board.

## Electrical: ALDO4

The NC7WZ16 on pins 31/32 is powered from AXP2101 ALDO4 (which also pulls up
headphone detect into the STM32, 100 k). Its datasheet input-high threshold
is 0.70 x VCC, and the RK3506 pad's guaranteed output high is only 1.4 V
(about 1.7 V measured), so on paper a 3.3 V ALDO4 does not accept the signal.

**In practice it does**: at the stock ALDO4 setting a 1.8 V square wave from
GPIO4_B2 comes through loudly. A typical part switches near half its supply,
so this works on typical figures with little margin; it may vary with
temperature and between units. If a unit turns out marginal, lowering ALDO4
toward 2.1 V in the STM32 firmware (AXP2101 register `0x95` = (mV - 500) / 100)
restores margin, at about 4 dB less output; the headphone-detect input still
reads high at that level on ST's design figure (1.55 V at 3.3 V VDD).
ALDO4's actual value has not been measured.

## Host side

- The hw pointer is what the M0 has played (its ring read index), not what
  has been queued. At most `buffer_size - period_size` frames are queued
  ahead, so the pointer cannot move a whole buffer between two looks.
- `period_bytes_max` is half the ring.
- `sync_stop` waits for the worker and the timer before the PCM buffer can be
  freed.
- The M0 is booted on START and shut down on STOP (a firmware load each
  time). Keeping it alive between streams is future work.

## Checks

```
make                 # firmware, arm-none-eabi-gcc;  PROFILE=1 for the profiling build
make check           # check_ring (host copy maths), check_dsm (modulator)
make check-emu       # runs m0_play() from the ELF in Unicorn against a model;
                     # needs: pip install unicorn pyelftools
```

`check-emu` compares every GPIO write over 900,000 ticks at three tick
ratios: silence, DC, sine, noise across the ring wrap, rail-to-rail input
(clamp), underrun hold, an unaligned start index, clean return on STOP and
preserved registers. It checks arithmetic and schedule, not timing.

## Open issues

1. **Speed.** Running from SRAM over the bus costs 2.5 cycles per
   instruction, which is what limits the tick to 600 kHz. As TCM the same
   code would run at 1 cycle per instruction and the tick could roughly
   double. With the stock OP-TEE, TCM means loading at `0xFFF84000`, after
   which Linux cannot touch that SRAM again until reboot: the firmware could
   be loaded once per boot and never reloaded, and the ring could not stay in
   that SRAM. Not attempted.
2. **Stalls.** About one tick per host ring update takes far longer than
   normal (roughly 45 per second with 1024-frame periods, 110 with 256), at
   any tick rate. The cause is Linux touching the SRAM while the M0 is
   fetching code from it. Putting the ring in the neighbouring SRAM bank
   (`0xFFF84000`) changes nothing, so the banks share one bus port; only TCM
   (a private port for the M0) would take the code off that path. Each stall
   delays a single edge.
3. **Quality.** Not measured. Nobody has yet listened critically or looked at
   the output on a scope or analyser.
4. **Sample timing.** Sample changes are snapped to the tick grid (a sample
   lasts 13 or 14 ticks at 600 kHz). Choosing a bus clock that is a multiple
   of 48 kHz (the 1179.648 MHz audio PLL) would make that exact; it changes
   the bus clock for the whole SoC.
5. **ALDO4 margin**, above.
