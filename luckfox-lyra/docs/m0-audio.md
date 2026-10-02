# PicoCalc M0 audio on the Luckfox Lyra (RK3506)

Status: **plays on hardware** (Luckfox Lyra RK3506G2 in a PicoCalc,
Calculinux 6.1.99, 2026-10-02): test tones through the ALSA device, at a
3 MHz output bit rate with the firmware running as TCM, or 1 MHz in bus
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
| highest tick rate at which every tick fits (before interpolation was added) | 1.2 MHz | 3.2 MHz |
| ticks stretched by other bus traffic | about one per Linux ring update | a few tens a second |
| firmware reload | any time | **not until reboot** |

In TCM mode OP-TEE switches the SRAM from `0xFFF84000` to `0xFFF8C000` to the
M0's private port; Linux reads zeros from it afterwards and there is no call
to switch back. The loader therefore loads the image once, pins itself in
memory, and on later stop/start only resets the M0, which re-runs the image
still in the TCM. A warm reboot returns the SRAM to bus mode (the next boot's
DDR-init stage loads into it again). The `m0-audio` overlay selects TCM mode;
remove `rockchip,tcm` and set `tick-rate-hz = <1000000>` to develop firmware
without rebooting.

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
at 600 kHz (with an earlier, slower loop), because M0 accesses to DDR are slow.

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
| GPIO4 DR write, as the M0 sees it | 32, or 7 with bufferable writes | 2 |
| GPIO4 DR write then read of the same register | 63, or 59 with bufferable writes | 4 |
| TIMER0 or GPIO4 register read | 30-32, constant | 2 |
| core-internal register (NVIC, SysTick) | 2 | 2 |
| SRAM read at its absolute address / through the address-0 window | 6 / 2 | 2 |

(Each figure includes about two cycles of the measurement itself.)

`hclk_m0` is a gate on `aclk_bus_root` (`CRU_CLKSEL_CON21`): the M0 runs at
the bus clock and cannot be raised on its own. `clk_gpll_div_100m` is
93.75 MHz here, not 100, and Linux gates it when it has no user.

### Bufferable writes

`GRF_SOC_CON0` bit 12, `mcu_hprot_bufferable`, makes the interconnect
acknowledge the M0's bus writes at once and complete them behind its back. A
GPIO write then holds the M0 for about 5 cycles instead of 30. The write
still lands at a fixed time after its instruction: a read of the same
register queued behind it returns after 59 cycles in 4093 of 4096 samples,
against 63 without the bit. The firmware sets the bit at start-up and runs
the modulators while the write completes; the first bus access of any
per-sample step comes late enough in the tick not to queue behind it.
(Whether the pin edge itself sits at a constant delay has only been inferred
from that read-back timing, not seen on a scope.)

## Tick: SysTick and WFI

The firmware takes no interrupts. PRIMASK stays set; SysTick's exception is
enabled, so when it becomes pending it wakes the core from WFI and is never
taken. Measured:

- wake is a constant 3 cycles after the SysTick reload;
- consecutive timer-woken wakes are exactly the same number of cycles apart
  (zero spread over 4096);
- no spurious wakes in thousands of ticks.

The GPIO write is the first instruction after the wake, so every output edge
lands the same number of core cycles after its tick. Polling a TIMER0 status
bit instead costs 30 cycles per look and jitters by about 280 ns; TIMER0 also
needs its clock parent selected and enabled by Linux.

The driver tells the firmware how many core cycles a tick is and how many
ticks a sample lasts (a whole part plus a 32-bit fraction whose carries add a
tick), from `clk_get_rate()` of `hclk_m0` and its `tick_hz` parameter (also
`tick-rate-hz` in the device tree). `tick_hz` can be changed at runtime and
applies from the next stream, so rates can be tried without touching the
firmware.

**A tick must be a whole number of GPIO clock periods.** The GPIO block is
clocked by `pclk_gpio4`, 93.75 MHz, half the M0's 187.5 MHz, so a pin can only
change on every second core cycle. With an odd count (the 63 cycles that
3 MHz first came out as) every other bit is a core cycle, 5.3 ns, late. A
regular error like that brings the modulator's ultrasonic noise down into
the audio band: in simulation the noise under a -40 dBFS tone goes from
-79 dBFS to -40 dBFS. The driver therefore makes the tick a multiple of the
ratio of the two clocks, which it reads (`clocks = core, pin` in the device
tree): 2 as the SoC boots, 4 with the core clock doubled (below).
`tick_even`, on by default, can be turned off to hear the difference. By ear
this is what removed the hiss.

### A faster core clock

`hclk_m0` is `aclk_bus_root` (which also clocks the system SRAM and the DMA
controllers), and that is `clk_gpll_div`, GPLL / 8, undivided. `clk_gpll_div`
also feeds the low-speed peripheral bus and several peripheral clocks, each
through a divider of its own. Halving every one of those and then setting
`clk_gpll_div` to GPLL / 4 leaves them all where they were and puts the M0 at
375 MHz, with the GPIO clock exactly a quarter of it. `picocalc_m0_diag_fw/
m0clk` is a module that does that through the clock framework when loaded
and undoes it when removed. **An experiment**: the TRM gives no limit for this
clock.

Measured on the board at 375 MHz, firmware as TCM:

| | 187.5 MHz | 375 MHz |
|---|---|---|
| straight-line instruction | 1 cycle | 1 cycle |
| SRAM read, absolute address | 7 cycles (37 ns) | 7 cycles (19 ns) |
| GPIO write, blocking | 31 cycles (165 ns), no jitter | 49 cycles (131 ns), no jitter |
| WFI wake jitter | 0 | 0 |
| core cycles in a 3.02 MHz tick | 62 | 124 |

The audio firmware plays at 375 MHz with a 124-cycle tick and keeps exact
pace; the display and its DMA work; the delays a display flood causes are
the same in nanoseconds. That is all the testing it has had. The board
**hung once** at 375 MHz, while the display's SPI controller was being
unbound and bound to another driver (the same operation had worked at
375 MHz minutes earlier); whether the clock was the cause is not known.
Nothing is known about margins, temperature or power.

What it would buy: the uncorrected loop needs 44 to 56 of the 124 cycles. A
correction for late pin writes (below) needs the blocking write, 49 cycles
here, and about 70 more, which is close to fitting at 3 MHz.

## Playback loop (`play.S`)

`m0_play()` owns every register, SP included (it points at the loop's small
state block, so loads from it need no address register), and keeps both
modulators' state in registers. A tick is: WFI, **write the GPIO word
computed on the previous tick**, clear the pending bit, run both modulators.

The per-sample work is cut into fifteen steps of at most about a dozen
cycles. The six that use the shared SRAM come first (stop check; snapshot
`write_idx`; move to the next frame unless the ring is empty; fetch; compute
`read_idx`; publish it), so the host knows when the SRAM is not in use; then
sample length, two steps per channel to work out where the input starts and
what it steps by, one per channel to load those, one per channel to clamp.
Each of the first fifteen ticks of a sample carries one step as
straight-line code, so no tick is much longer than a plain one and nothing
is dispatched; the rest of the sample is plain ticks in a counted loop. Left
and right change one tick apart. The clamp runs once per sample rather than
every tick. A `PROFILE=1` build has three more steps, after the SRAM ones.

### Between samples

The modulator's input does not jump from one sample to the next: every tick
it moves a step towards the new sample. Everything is kept in units of
2^-shift of an input LSB, 2^shift being the smallest power of two a sample's
ticks fit in (64 at 3 MHz); the step is then simply the difference between
the two samples, with no multiply and no rounding, and after n ticks the
input has gone n/2^shift of the way. It is set exactly every sample, so
nothing accumulates; what is left of the way at the end of the sample (1/64
at 62 cycles per tick, where a sample is 63 ticks) is taken in one go. The
output is one sample later than before.

What that buys, simulated at 62 cycles per tick with a 1 kHz tone: the
images at 47 and 49 kHz go from -72 to -89 dBFS at -40 dBFS, and from -35 to
-67 dBFS at -3 dBFS. **The noise in the audio band does not change**
(-79 dBFS and -68 dBFS respectively): holding a sample adds images, not
hiss. Linear interpolation also rolls the top of the band off a little more
than holding does (about 5 dB at 20 kHz instead of 2.6, 0.6 dB more at
10 kHz). It tames the modulator, too: on full-scale random squares the
second integrator peaks at 15 times full scale, against 400 when holding.

`interp=0` on the sound module makes the firmware hold each sample, to
compare (next stream). More than 64 ticks per sample (a tick shorter than
62 cycles) also means hold: the state has no room for a larger shift. That
makes 62 cycles, 3.02 MHz, the fastest tick worth using at 187.5 MHz.

Measured with a `PROFILE=1` build (every tick records the longest tick so far
and whether it ran into the next; the driver prints both when a stream ends;
the profiling itself costs about 17 cycles per tick), two-second tone:

| Mode | `tick_hz` | Cycles per tick | Ticks that overran |
|---|---|---|---|
| bus | 1.0 MHz | 188 | 95 (stretched ticks only) |
| bus | 1.2 MHz | 156 | 195 |
| bus | 1.4 MHz | 134 | 68 % of all ticks |
| TCM | 2.0 MHz | 94 | 24 |
| TCM | 3.0 MHz | 63 | 27-71 |
| TCM | 3.2 MHz | 59 | 80 |
| TCM | 3.3 MHz | 57 | 6 % of all ticks |
| TCM | 3.6 MHz | 52 | 41 % |

Those figures are from before interpolation, which costs four more cycles
per tick. With it, in TCM: a `PROFILE=1` build fits in 64 cycles and not in
62; a normal build, about 18 cycles lighter, keeps exact pace at 62 (0 us
behind over 10 s). So profile at `tick_hz=2929688` or below. Bus mode has
not been run since.

An overrun delays that tick's edge and the next tick starts late; ticks are
not lost unless the work exceeds two ticks. For comparison, the first working
loop (interrupt-free but with the per-sample work on four ticks, a clamp on
every tick and unbuffered writes) fitted at 600 kHz in bus mode and 1.2 MHz
as TCM.

## Modulator

Per channel, `FS = 32768 << shift`, `G = +FS` when the output bit is 1, else
`-FS - 1` (one's complement: an offset of 2^-shift LSB, and one instruction
fewer than a shift that depends on `shift`):

```
i1 += x - G_prev;  x += dx;  y = i2 + i1;  bit = (y >= 0);  G = bit ? FS : ~FS;  i2 = y - G
```

with `i2` clamped once per sample. A standard second-order modulator with
unity signal gain. Both stages use the same full-scale feedback; input is
scaled by 7/8; the clamp is there because scaling alone does not stop
full-scale noise from running the state into int32 wrap. `check_dsm.c`
mirrors the arithmetic and asserts this.

Simulated in-band SNR (1 kHz tone, 20 Hz-20 kHz, ideal edges) is about 49 dB
at -3 dBFS for a 1 MHz tick, 60 dB at 2 MHz and 65 dB at 3 MHz; under a
-40 dBFS tone the noise at 3 MHz is -79 dBFS. Edges are not ideal. Simulated
at 3 MHz under the -40 dBFS tone:

| Pin timing | In-band noise |
|---|---|
| ideal | -79 dBFS |
| rising edges 1 ns later than falling ones | -69 dBFS |
| every other bit 5.3 ns late (odd cycles per tick, see above) | -40 dBFS |

Rise/fall asymmetry of the 1-bit output costs more the more edges there are,
so the best-sounding rate may be below the fastest that fits; `tick_hz` is
there to find out. None of this is a measurement of the board's output.

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
  every frame), not what has been queued. At most `buffer_size - period_size`
  frames are queued ahead, so the pointer cannot move a whole buffer between
  two looks.
- `period_bytes_max` is half the ring.
- The frame before the start position is zeroed at stream start: it is what
  the firmware plays until it has fetched a frame, and what it repeats on an
  underrun there.
- `sync_stop` waits for the worker (and with it the M0's return to idle) and
  the timer before the PCM buffer can be freed.

## Checks

```
make                 # firmware, arm-none-eabi-gcc;  PROFILE=1 for the profiling build
make check           # check_ring (host copy maths), check_dsm (modulator)
make check-emu       # runs m0_play() from the ELF in Unicorn against a model;
                     # needs: pip install unicorn pyelftools
```

`check-emu` compares every GPIO write over more than a million ticks at
three tick rates: silence, DC, sine, noise across the ring wrap, rail-to-rail
input (clamp), underrun hold, an unaligned start index, clean return on STOP
and preserved registers. It checks arithmetic and schedule, not timing; the
idle loop and the handshake are only tested on hardware.

## Diagnosing ticks and dropouts

A `PROFILE=1` firmware also keeps an event log in the SRAM after the ring:
every tick that was still working when the next fell due, with the step of
the sample it was in, and every sample for which the ring was empty, each
with its sample number. `m0trace` (in the firmware directory; build it for
the board) reads it from Linux and shows the events by kind, a 50 ms
timeline, the gaps between them and, with `-p FRAMES`, where in the ALSA
period they fall:

```
make PROFILE=1                         # then load it (bus mode, or TCM after a reboot)
aplay something.wav; m0trace -p 1024
```

The log says when a tick was late, not by how much. For that the driver
compares the frames the M0 played with the time they should have taken, with
any firmware, and prints it when a stream ends:

```
M0 played 479232 frames in 9984000 us: 0 us behind (0 us per second)
```

The two clocks come from the same crystal, so "behind" is time the M0 spent
stalled (or, in mid stream, with an empty ring, which the log shows
separately), good to one frame, 21 us. A few stretched ticks a second of a
fraction of a tick each are below that; stalls long enough to hear are not.

The log is a ring of 256 entries and the end of a stream fills it (the
ring runs dry, and at 3 MHz logging that makes ticks late in itself), so
read it while the stream is still playing.

### What the ticking was

Measured in TCM at 3 MHz, 10 s of digital silence, period 1024:

| Host activity | Late ticks | M0 behind |
|---|---|---|
| driver as it was (one copy per period) | 10-25 a second | 10-16 us/s |
| the same, plus reading an SRAM word 13 million times a second | 11 a second | 11 us/s |
| the same, plus reading a GRF register 6 million times a second | 15 a second | 5 us/s |
| the same, plus *writing* an SRAM word as fast as possible | 120000 a second | 18000 us/s |
| driver writing only in the M0's quiet ticks (`ring_sync`, now the default) | 0-1.5 a second | 0-2 us/s |

Host writes to the SRAM hold up the M0's accesses to it; host reads do not.
The M0 uses the SRAM only in the step ticks at the start of each sample, and
a period's copy takes about 10 us, less than a sample, so most copies fell
wholly in the plain ticks and did no harm. But the driver's timer runs on
Linux's clock and drifts slowly through the M0's sample; every half second or
so a few consecutive copies landed on the step ticks, each stalling the M0
for a fraction of a microsecond: a pin held too long, a click, even in
silence. That was the cluster of about five ticks every half second, there
with silence, independent of the tick rate, and different with another ALSA
period. Turning off Wi-Fi or the keyboard's I2C polling made no difference.

The driver now watches `read_idx`, which the firmware writes in its last step
tick, and writes the ring (as device memory, word by word) only in the plain
ticks that follow, carrying on after the next sample's steps if it runs out
of time. That costs up to about 40 us per period with interrupts off.
`echo 0 > /sys/module/picocalc_snd_m0/parameters/ring_sync` switches it off
while playing, to hear the difference; the line `ring updates: ...` printed
when a stream ends says how many updates were synchronised. With too few
plain ticks in a sample (a slow tick in bus mode) there is no room and the
driver writes unsynchronised.

What is left with `ring_sync` on is the display. A few ticks are late at
multiples of 1.0133 s, which is how often the console's terminal program
redraws; stop it and the bare console cursor blinks instead, and the late
ticks move to multiples of 203 ms. The heartbeat LED and the battery
monitor make no difference. The cause is the same kind of thing as before,
on another bus: host *writes* to peripheral registers get in the way of the
M0's pin write, which it makes every tick.

| Host activity (`m0hammer`, `ring_sync` on) | Late ticks | M0 behind |
|---|---|---|
| nothing extra (terminal redrawing once a second) | 0-1.5 a second | 0-2 us/s |
| writing a GPIO data register (bank 0 or 4, masked no-op) 16.5 million times a second | 250000 a second | 341000 us/s |
| the same 1000 times a second | 1 a second | 1 us/s |
| writing `GRF_SOC_CON0` (masked no-op) 16.5 million times a second | as nothing extra | 0 |
| reading a GPIO or SPI register 5 million times a second | 0.4-2.7 a second | 0 |

So a host GPIO write costs the M0 about 21 ns when they meet, and a redraw
(SPI transfer plus the display's D/C pin on GPIO0) makes two to four
consecutive ticks late about once: pin edges tens of nanoseconds late, where
the ring copy made them late by most of a microsecond a dozen times a
second. Nothing in the driver can prevent this; whether it can be heard has
not been checked.

### Pops: what delays the pin write

By ear, with the hiss gone: faint random pops with an ear against the
speaker in normal use, and a lot of noise while the console is flooded with
text. The diagnostic firmware times one GPIO4 write, 4096 times over 44 ms
(`picocalc_m0_diag_fw`, writes not bufferable, TCM); normally every one
takes 31 core cycles:

| Linux doing | M0's GPIO write |
|---|---|
| nothing much | 31 cycles, 4096 of 4096 |
| console flooded with text (display on SPI0 with DMA, D/C on GPIO0) | 75 % at 31, the rest up to 103 (0.38 us late), mean +9 |
| reading the SD card on SPI1 (`mmc1`, mmc-spi) | 80-85 % at 31, the rest later, as with the display |
| writing a GPIO data register as fast as possible | mean 411, up to 657 (3.3 us late); 2 % at 31 |
| reading 61 MB from the SD card on the SDMMC controller (`mmc0`) | 31, 4096 of 4096 |
| flood ping over Wi-Fi (USB) | 31, 4095 of 4096 |

So traffic through the SPI controllers and Linux's GPIO writes hold up the
M0's pin write, far longer than one host write takes; the SDMMC controller
and USB do not. The TRM has no priority setting for the M0 on the bus. In
normal use it is the display that matters: each redraw (the console's
terminal program once a second, every keypress echoed) is a burst of late
edges, a click. Simulated at 3 MHz under a -40 dBFS tone, a quarter of the
pin writes late by up to 72 cycles puts the in-band noise at -22 dBFS for as
long as it lasts; 2 % late, -32 dBFS.

The M0 can see the delay (the write returns late when it is not
bufferable), so the firmware could measure each one and put what a late
edge cost back into the modulator, which shapes the error out of the audio
band: simulated, -22 dBFS becomes -70. A first version is on the branch
`m0-audio-comp-wip` and **does not work**: it corrects only on late ticks, in
more cycles than a tick has to spare, so one late tick makes the next late
or loses it. A working one has to do the correction on every tick, with the
pin write blocking (25 cycles more than now), and needs a tick of about 128
core cycles: 1.46 MHz, with the quiet-system noise floor near -64 dBFS
instead of -79.5. Not worth it as it stands; it depends on getting the loop
shorter first.

**The display: DMA burst length.** The kernel's `spi-rockchip` sends pixel
data by DMA in bursts of a quarter of the FIFO, and the M0's pin write waits
out a whole burst. Measured as above during a console flood, with a copy of
the driver built as a module that takes the burst length as a parameter:

| TX DMA burst (16-bit words) | M0 writes late | Worst | Mean |
|---|---|---|---|
| 16 (the driver's own) | 25 % | +72 cycles | +8.7 |
| 4 | 27 % | +40 | +5.4 |
| 2 | 22 % | +24 | +2.8 |
| 1 | 16 % | +16 | +1.3 |

From those figures a burst of 1 takes the timing-error power during display
activity down by about 15 dB, or about 11 dB per redraw, since redraws then
take 2.6 times as long. The change is one line in the kernel
(`dst_maxburst` in `rockchip_spi_prepare_dma()`), not in this repository;
by ear it is not yet judged. Each beat can still hold the M0 for up to 16
cycles, so this reduces the pops and does not remove them.

The display cannot be moved off this bus: its pins are on the routable pin
matrix, but the matrix only offers SPI0 and SPI1, both on the same
low-speed peripheral bus as GPIO4 (the SoC's "SPI2" is a slave-only
SPI-to-APB bridge on fixed pins).

**Measuring lateness without a blocking write does not work.** With
bufferable writes a second GPIO write straight after the first returns at
once (two take 8 cycles): the write buffer holds several, so a later write
says nothing about whether an earlier one has landed.

**Where the cycles go** (TCM, from the disassembly and the measured costs):
wake 3, pin write 7, clearing SysTick's pending bit 6, both modulators and
the next pin word 21: 37 for the core of a tick. A plain tick is 44 with
the loop; a step tick 49 to 56. The tick is 62, set by the step ticks and,
for interpolation, by a sample having to fit in 64 ticks; there is little
to gain for plain playback. A correction for late pin writes would add the
blocking write (26) and about 35 of arithmetic on every tick; see "A
faster core clock" for what doubling the M0's clock does to that.

`poptest.sh` in the test directory on the board plays silence in phases
marked by beeps while it freezes the display, stops the keyboard polling,
takes Wi-Fi down and floods the console, for telling by ear which matters.
It stops the console's terminal service, which kills everything started
from that console: it must be started through `systemd-run` or over SSH.

Earlier observations, for the record: the same number of late ticks with the
heartbeat LED trigger on or off; in simulation the modulator produces no
burst of in-band noise when its input drops to digital silence.

## Open issues

1. **Hiss**: gone by ear with the even tick (it was plainly audible at 63
   cycles per tick).
2. **Quality.** Not measured. Nobody has yet listened critically or looked at
   the output on a scope or analyser, at any tick rate.
3. **Pin timing with bufferable writes** is inferred, not observed (above).
4. **Suspend.** Untested in either mode: what suspend-to-RAM does to the TCM
   contents and the M0, and whether anything needs the boot-stage code this
   design overwrites in the first SRAM bank.
5. **More speed.** A tick is now about 40 cycles as TCM, half of it the two
   modulators. What is left is running the modulator on the A7 (the M0 would
   only shift out a precomputed bit stream), which costs A7 time, or raising
   `aclk_bus_root` to 250 MHz (GPLL / 6), which adds a third but changes the
   bus clock for the whole SoC.
6. **Sample timing.** Sample changes are snapped to the tick grid. A bus
   clock that is a multiple of 48 kHz (the 1179.648 MHz audio PLL) would make
   that exact; same caveat.
7. **Pops from late pin writes**: display redraws, the SPI SD card and
   Linux's GPIO writes delay the M0's pin write; see "Pops: what delays the
   pin write". Audible as faint random pops in normal use. A correction in
   the firmware is possible in principle and too expensive in cycles as the
   loop stands.
8. **Bus mode** has not been run with `ring_sync` or with interpolation;
   the measurements are all TCM.
9. **Pop when a stream ends** (and presumably when it starts): the pins go
   from the 50% pattern of silence to low, a DC step through the output
   capacitor. Needs a ramp between the two.
10. **ALDO4 margin**, above.
