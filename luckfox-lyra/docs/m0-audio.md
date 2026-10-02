# PicoCalc M0 audio on the Luckfox Lyra (RK3506)

Status: **plays on hardware** (Luckfox Lyra RK3506G2 in a PicoCalc,
Calculinux 6.1.99, 2026-10-02): test tones through the ALSA device, at a
1.2 MHz output bit rate with the firmware running as TCM, or 600 kHz in bus
mode. Audio quality has not been measured or judged yet; see
[Open issues](#open-issues).

Sources: RK3506 TRM Part 1 V1.2, RK3506G2 datasheet V1.5, the PicoCalc
mainboard V2.0 schematic, the upstream M0 loader
([nvitya/rk3506-mcu](https://github.com/nvitya/rk3506-mcu)), and measurements
taken on the board with [`picocalc_m0_diag_fw`](../picocalc_m0_diag_fw/README.md)
and the firmware's `PROFILE=1` build.

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

## Memory and the two ways to run the firmware

The Cortex-M0 has no VTOR and fetches its vectors from address 0. The SoC's
address converter maps M0 address 0 onto the *code start address* passed to
Rockchip's SiP call `0x82000028`, so the firmware is **linked at 0** and
`picocalc_rk3506_rproc` places it at that address. (The core could not
execute at `0xFFF8xxxx` anyway: on ARMv6-M everything from `0xE0000000` up is
execute-never.) The same image runs in either mode:

| | Bus mode | TCM mode (`rockchip,tcm` on the loader node) |
|---|---|---|
| image at | `0xFFF88000` | `0xFFF84000` |
| code fetch | over the SoC bus | private port: tightly-coupled memory |
| straight-line instruction | 2.5 cycles | 1 cycle |
| heaviest tick | about 295 cycles | about 155 cycles |
| highest tick rate that fits | 600 kHz | 1.2 MHz |
| ticks stretched by other bus traffic | about one per Linux ring update | under ten a second |
| firmware reload | any time | **not until reboot** |

In TCM mode OP-TEE switches the SRAM from `0xFFF84000` to `0xFFF8C000` to the
M0's private port; Linux reads zeros from it afterwards and there is no call
to switch back. The loader therefore loads the image once, pins itself in
memory, and on later stop/start only resets the M0, which re-runs the image
still in the TCM. The `m0-audio` overlay selects TCM mode; remove
`rockchip,tcm` and set `tick-rate-hz = <600000>` to develop firmware without
rebooting.

System SRAM, `0xFFF80000..0xFFF8C000`, three 16 KB banks behind one bus port:

| Address | Use |
|---|---|
| `0xFFF80000` (4 KB) | OP-TEE; not readable from Linux |
| `0xFFF81000` (64 B + 8192 B) | `m0_audio_shmem_t` header and ring |
| `0xFFF84000` / `0xFFF88000` | firmware image: TCM mode / bus mode |

The ring is in the first bank because that is the one that stays on the
ordinary bus in TCM mode. At boot that area holds the remains of the
DDR-init stage (a 17 KB+ image loaded at `0xFFF81000`); nothing uses it
afterwards as far as is known, and both this and the upstream loader
overwrite it. **Suspend-to-RAM has not been tested with it overwritten.**
Keep the ring in SRAM, not DDR: with the ring in DDR 30 % of ticks overran
at 600 kHz, because M0 accesses to DDR are slow.

## Keep-alive and idle

The firmware is booted at the first stream and never shut down between
streams (in TCM mode it could not be loaded again). Idle, it sets SysTick to
one wake every 2^19 core cycles (2.8 ms), sleeps in WFI in between, and looks
at the control word on each wake; `GRF_SOC_STATUS2` shows the M0 asleep in
100 % of samples while idle. Streams are handed over through the header:

```
host: wait for m0_state == IDLE, fill in the header, ctrl = PLAY
M0:   sees PLAY at its next idle wake, m0_state = PLAY, SysTick to the tick rate, plays
host: ctrl = STOP
M0:   sees STOP within one sample, pins low, SysTick slow, m0_state = IDLE
```

## What things cost on this SoC

Measured at `hclk_m0` = 187.5 MHz (GPLL 1500 MHz / 8):

| | Cycles (bus mode) | With no wait states |
|---|---|---|
| straight-line instruction | 2.5 | 1 |
| two-instruction loop with a taken branch | 10 | 4 |
| GPIO4 DR write, start to finish | 33, constant | 2 |
| TIMER0 register read, start to finish | 30, constant | 2 |
| core-internal register (NVIC, SysTick) | 2 | 2 |
| SRAM read at its absolute address / through the address-0 window | 6 / 2 | 2 |

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

The driver tells the firmware how many core cycles a tick is and how many
ticks a sample lasts (whole part plus a remainder spread Bresenham-style),
from `clk_get_rate()` of `hclk_m0` and its `tick_hz` parameter (also
`tick-rate-hz` in the device tree). `tick_hz` can be changed at runtime and
applies from the next stream, so rates can be tried without touching the
firmware.

## Playback loop (`play.S`)

`m0_play()` owns every register, SP included (it points at the loop's small
state block, so loads from it need no address register), and keeps both
modulators' state in registers. A tick is: WFI, **write the GPIO word
computed on the previous tick**, clear the pending bit, run both modulators.

The per-sample work is cut into nine steps of at most about a dozen cycles
(stop check; sample length; snapshot `write_idx`; move to the next frame
unless the ring is empty; fetch; left sample in and clamp; right sample in
and clamp; compute `read_idx`; publish it). Each of the first nine ticks of a
sample carries one step as straight-line code, so no tick is much longer
than a plain one and nothing is dispatched; the rest of the sample is plain
ticks in a counted loop. Left and right change one tick apart. The clamp
runs once per sample rather than every tick: simulated with the worst inputs
the state then peaks at 2^24, against a wrap at 2^31.

Cycle counts from the listing, as TCM: about 60 for the tick itself, 65 for a
plain tick with its loop count, at most about 75 for a step tick. Not yet
measured on the board.

The figures below are for the **previous** loop (per-sample work on four
ticks, clamp on every tick), measured with its `PROFILE=1` build (the loop
records the longest tick and counts overruns; the driver prints them when a
stream ends), two-second tone:

| Mode | `tick_hz` | Cycles per tick | Ticks that overran |
|---|---|---|---|
| bus | 600 kHz | 313 | about 100 (stretched ticks only) |
| bus | 650 kHz | 288 | 7 % of all ticks |
| bus | 750 kHz | 250 | 32 % |
| TCM | 1.0 MHz | 188 | 14 |
| TCM | 1.2 MHz | 156 | 18 |
| TCM | 1.25 MHz | 150 | 0.5 % (the tick that publishes `read_idx`) |
| TCM | 1.3 MHz | 144 | 4 % |
| TCM | 1.5 MHz | 125 | 15 % |
| TCM | 2.0 MHz | 94 | 69 % (plain ticks no longer fit) |

An overrun delays that tick's edge and the next tick starts late; ticks are
not lost unless the work exceeds two ticks.

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
at -3 dBFS for a 1 MHz tick, changing by about 12 dB per doubling: roughly
40 dB at 600 kHz and 52 dB at 1.2 MHz. That is a simulation of the modulator
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

- The hw pointer is what the M0 has played (its ring read index, published
  every frame), not what has been queued. At most `buffer_size - period_size` frames are queued
  ahead, so the pointer cannot move a whole buffer between two looks.
- `period_bytes_max` is half the ring.
- `sync_stop` waits for the worker (and with it the M0's return to idle) and
  the timer before the PCM buffer can be freed.

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
preserved registers. It checks arithmetic and schedule, not timing; the idle
loop and the handshake are only tested on hardware.

## Open issues

1. **Reboot after TCM.** Not yet tested: that a warm reboot returns the SRAM
   to bus mode (the next boot's DDR-init stage loads into it), and that the
   overlay then loads the firmware again. Others report that it does.
2. **Suspend.** Untested in either mode: what suspend-to-RAM does to the TCM
   contents and the M0, and whether anything needs the boot-stage code this
   design overwrites in the first SRAM bank.
3. **Quality.** Not measured. Nobody has yet listened critically or looked at
   the output on a scope or analyser.
4. **More speed.** Of the roughly 60 cycles in a tick, the GPIO write is 33
   and cannot be shortened, and the two modulators are 19. What is left is
   running the modulator on the A7 (the M0 would only shift out a precomputed
   bit stream), which costs A7 time, or raising `aclk_bus_root` to 250 MHz
   (GPLL / 6), which adds a third but changes the bus clock for the whole SoC.
5. **Sample timing.** Sample changes are snapped to the tick grid. A bus
   clock that is a multiple of 48 kHz (the 1179.648 MHz audio PLL) would make
   that exact; same caveat.
6. **Residual stretched ticks** in TCM mode (under ten a second): unidentified.
7. **ALDO4 margin**, above.
