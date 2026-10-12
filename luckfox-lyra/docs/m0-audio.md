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
375 MHz, with the GPIO clock exactly a quarter of it. The remoteproc driver
does that, through the clock framework, when its node has
`picocalc,double-core-clock` (the m0-audio overlay as shipped has it), and
undoes it when unbound; while it is bound none of those other clocks can be
set above its old rate (their parent being twice as fast, a driver could
otherwise be handed twice the clock it expects). **An experiment**: the TRM
gives no limit for this clock. (It was first tried as a separate module,
`m0clk`, which this replaced.)

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
the same in nanoseconds. A minute with audio playing, the console flooded
and the SPI SD card being read passed without kernel errors (51.7 C), and
the clock was switched up and down several times with the M0 running. That
is all the testing it has had. The board **hung once** at 375 MHz, while the
display's SPI controller was being unbound and bound to another driver, with
an earlier version of the module that did not yet cap the other clocks (the
same operation had worked at 375 MHz minutes earlier); whether the clock was
the cause is not known. Nothing is known about margins or power.

What it buys: the uncorrected loop needs 44 to 52 of the 124 cycles, and
there is room for the loop that corrects late pin writes ("Correcting for
late pin writes"), which is what it is for.

Binding the display's SPI controller to another driver with the clock
already raised hung the board both times it was tried; doing that first and
raising the clock afterwards has worked every time.

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

## Checking the output level

`m0level` (firmware directory; build it for the board) reads the two pins
back from Linux while a stream plays and prints how much of the time each
is high: the level the firmware is really producing, whatever the analogue
side does with it. For a 16-bit sample x the duty should be
0.5 + x * 7/8 / 65536. Measured, interpolating or holding, at a 187.5 or a
375 MHz core clock: a DC level of +16384 / -8000 gives 0.7187 / 0.3932
(0.7188 / 0.3932 expected), and a tone of amplitude 20000 an amplitude of
0.2670 at 100 Hz and 0.265 at 1 kHz (0.2670 expected). Interpolation reads
0.6 dB lower than holding at 10 kHz, as it should.

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

What is left with `ring_sync` on is traffic on the SPI controllers. A few
ticks are late at multiples of 1.0133 s. That was first put down to the
console's terminal program redrawing, wrongly: it is the kernel asking the
card in the SPI slot (`mmc1`) whether it is still there, once a second (see
"An idle card ticked once a second" below). With the terminal program stopped the
bare console cursor blinks, and there are late ticks at multiples of 203 ms
as well. The heartbeat LED and the battery monitor make no difference. The cause is the same kind of thing as before,
on another bus: host *writes* to peripheral registers get in the way of the
M0's pin write, which it makes every tick.

| Host activity (`m0hammer`, `ring_sync` on) | Late ticks | M0 behind |
|---|---|---|
| nothing extra (SD card on SPI polled once a second) | 0-1.5 a second | 0-2 us/s |
| writing a GPIO data register (bank 0 or 4, masked no-op) 16.5 million times a second | 250000 a second | 341000 us/s |
| the same 1000 times a second | 1 a second | 1 us/s |
| writing `GRF_SOC_CON0` (masked no-op) 16.5 million times a second | as nothing extra | 0 |
| reading a GPIO or SPI register 5 million times a second | 0.4-2.7 a second | 0 |

So a host GPIO write costs the M0 about 21 ns when they meet, and one poll
of the SD card (a short SPI transfer) makes two to four consecutive ticks
late about once: pin edges tens of nanoseconds late, where
the ring copy made them late by most of a microsecond a dozen times a
second. Nothing in the sound driver can prevent this. It can be heard: on
headphones, as a faint tick a second in silence, even with the correction
below.

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
and USB do not. The TRM has no priority setting for the M0 on the bus, and
the undocumented ones change nothing (see "Bus priorities" below). In
normal use it is the display that matters: each redraw (every keypress
echoed, every frame of a game) is a burst of late edges, a click. Simulated at 3 MHz under a -40 dBFS tone, a quarter of the
pin writes late by up to 72 cycles puts the in-band noise at -22 dBFS for as
long as it lasts; 2 % late, -32 dBFS.

### Bus priorities

The interconnect has priority (QoS) registers the TRM does not list. They
are where the RK3308 has its "Interconnect Service" region, in the layout
Rockchip's U-Boot uses on other chips (`+0x08` priority, two 3-bit levels;
`+0x0c` mode; `+0x10` bandwidth; `+0x14` saturation). Found by reading
(2026-10-09); nothing in U-Boot, the kernel or Rockchip's HAL for the
RK3506 touches them:

| Address | Blocks | Priority as booted |
|---|---|---|
| `0xff5c0100` | 1 (the CPU's, by the RK3308's layout) | `0x202` |
| `0xff5c8000`, `+0x400`... | 4 of other kinds (error log, probes) | - |
| `0xff5d8000`...`0xff5d8300`, `0x80` apart | 7 | `0x101 0x303 0x303 0x101 0x404 0x404 0x101` |
| `0xff5e0000`...`0xff5e0200`, `0x80` apart | 5 | `0x303` each |

**Reading `0xff5d0000` or `0xff5e8000` hangs the board** (no bus error, the
read never returns; power cycle). `aclk_vio_root` is gated, which may be why
for one of them. Everything else from `0xff588000` to `0xff5f0000` gives a
bus error.

They do not help. Measured by how far the M0 falls behind while 10 s of
silence plays and the SPI SD card is read (1.7 MB/s; `comp` on, 375 MHz):
26 ms a second as booted (25-66), against 2 us a second with no read. Then
with each of the three priority-1 blocks at `0xff5d8000` raised to 5 alone,
all three at 7, those at 7 with the two priority-4 blocks at 0, and those
at 7 with every other block there and the CPU's at 0: 25.3 to 26.4 ms a
second every time. Whichever block is the M0's, if any is, its priority
does not decide who gets the peripheral bus. The five blocks at
`0xff5e0000` were not tried.

### Correcting for late pin writes

The M0 can see the delay: with its bus writes not bufferable, a pin write
returns when the pin has changed, and the SysTick count read after it says
when that was. `m0_play_comp()` in `play.S` is a second copy of the playback
loop built on that. Every tick it works out how long the level that just
ended really lasted, from the counts of the two pin writes that bounded it,
and takes the difference from a tick, as a fraction of the level's
feedback, out of both integrators: the modulator then treats the bit as what
it really delivered and shapes the error out of the audio band. It does
this on every tick, late or not, with no branch on the lateness, so a tick
always costs the same; the tick is exactly 128 core cycles, which makes the
arithmetic shifts by constants. Because it measures from one write to the
next and not against the tick grid, it stays right when the loop has fallen
a tick or more behind the grid: it then just plays that much slower until it
has caught up. `picocalc,comp` on the sound node (the m0-audio overlay as
shipped has it) or `comp=1` on the sound module selects it
(default off; it needs all of the following).

What it needs, each established on the board:

1. **The M0 at 375 MHz** ("A faster core clock"): the blocking write takes
   49 cycles of the tick, and at 187.5 MHz a 128-cycle tick is 1.46 MHz.
   At 375 MHz it is 2.93 MHz.
2. **Cycles to spare on every tick.** A late write holds the loop up as well
   as the pin. If writes are late by more on average than the loop has
   spare, it falls further and further behind, which is far worse than not
   correcting. So this loop holds each sample (no interpolation), has 13
   steps and keeps its constants in registers: about 19 cycles spare on a
   plain tick, 12 on the longest step.
3. **Shorter DMA bursts in the display's SPI driver** (above), and the right
   length for the display's SPI clock. With the driver's own 16-word bursts
   the loop falls further and further behind while the display is redrawn.

The display's SPI clock on this board is 93.75 MHz (the overlay asks for
100 MHz; the driver's divider gives 93.75, 46.9, 31.25, 23.4, ...), and a
full 320x320 frame takes 20.6 ms: 48 frames a second. Continuous full-screen
redraws through KMS (`picocalc_m0_diag_fw/drmfps.c`) with corrected audio
playing, M0 at 375 MHz:

| Display SPI clock | DMA burst | Frames a second | Audio behind, per second |
|---|---|---|---|
| 93.75 MHz | 16 (the driver's own) | 48 | 20 ms |
| 93.75 MHz | 8 | 48.5 | 9.8 ms |
| 93.75 MHz | **4** | **48.4** | **0.05 ms** |
| 93.75 MHz | 2 | 39.8 | 18.8 ms |
| 93.75 MHz | 1 | 23.6 | 7.8 ms |
| 46.9 MHz | 16 | 26.2 | 5.7 ms |
| 46.9 MHz | 8 | 26.2 | 4.8 ms |
| 46.9 MHz | 2, 3 or 4 | 26.3 | 0.02-0.03 ms |
| 46.9 MHz | 1 | 23.7 | 7.1 ms |
| 31.25 MHz | 16 | 18.0 | 3.8 ms |
| 31.25 MHz | 1 or 4 | 18.0 | 0.02 ms |

Too long a burst holds the bus too long each time; too short a one cannot
keep the FIFO full, so the DMA never rests and the frame rate drops as well.
Four words keeps the stock frame rate with the loop all but on pace: under
this, the heaviest display load there is, it plays 0.005 % slow (a twelfth
of a cent), and since it measures from write to write nothing is left
uncorrected by that. (The 0.05 ms and 0.02 ms figures are the loop as it is
now; the rest of the table was measured with the version before, which
measured against the tick grid and has the same pace.)

How late the plain ticks' pin writes were during a console flood, one-word
bursts (`COMP_STATS=2` build, which has 7 cycles spare instead of 17):

| Display SPI clock | On time | Late by 64 cycles or more | Mean | Behind, over 10 s |
|---|---|---|---|---|
| (no flood) | 100.0 % | 0 | 0.0 cycles | 2 us |
| 93.75 MHz | 26 % | 21.75 % | 34 cycles | 102 ms |
| 31.25 MHz | 73 % | 0.03 % | 4.1 cycles | 0.15 ms |
| 23.4 MHz | 81 % | 0.01 % | 2.7 cycles | 0.10 ms |

In simulation the correction takes a redraw's noise under a -40 dBFS tone
from -36 dBFS to -72 (16 % of writes up to 32 cycles late). By ear, with an
earlier and worse version of the loop: a tone played through a console flood
was "not affected at all". The limit of the present loop is a write that
comes 100 core cycles or more later than a tick after the one before (0.27
us): that is misread. Four-word bursts hold a write up by 80 at most.

`check_play.py` runs this loop too, puts late writes in by setting the
SysTick count it reads, and checks both that the firmware matches the model
and, from the pin writes alone, that the late edges are cancelled (a model
sharing a sign error with the firmware would pass the first).

Debug build: `make COMP_STATS=1` adds up by how many cycles the pin writes
were pushed later, and the driver prints it when a stream ends (quiet
system: 0.02 cycles per tick; full-screen redraws at 48 a second: 3.5).
The lateness table above is from an earlier debug build that counted how
often each lateness against the tick grid occurred.

Where this lives: the SPI burst length is a kernel change,
`docs/spi-rockchip-tx-burst.patch` (the device tree property
`rockchip,tx-dma-burst`, which `picocalc-luckfox-lyra.dtsi` sets to 4 on
`&spi0`; it was measured with the same change as a module parameter of an
out-of-tree copy of the driver). The display's SPI clock, if it
is to be lowered, is `spi-max-frequency` in the board device tree. The
375 MHz clock and `comp` come with the m0-audio overlay.

**The SPI SD card is the worst offender.** While `mmc1` is being read the M0
does not just get late edges, it falls about 2 % behind (20 ms a second,
the same at 187.5 and 375 MHz): the SPI driver moves that card's data with
the CPU, in long runs of back-to-back register accesses, and the M0 is shut
out for whole ticks at a time. Not yet looked into.

**Reading the SPI SD card with four-word SPI1 bursts** (measured 2026-10-10,
kernel with `rockchip,tx-dma-burst` / `rx-dma-burst` = 4 on `&spi1`, card
clock 25 MHz, 3 MHz bit rate, TCM, 375 MHz core clock). `sd-audio-test.sh`
(firmware directory, as root on the board) plays 12-15 s of digital silence
while `dd` reads the raw card at about 1.6 MB/s and prints the driver's
"behind" figure for each SPI1 burst length. "Behind" is time the M0's
schedule slipped, and it does not mean the same in the two loops: the
uncorrected loop is paced by SysTick, so a late pin write is absorbed in the
loop's slack (about 70 cycles a tick at 375 MHz) and counts as nothing,
while the corrected loop has about 19 spare, so the same lateness slips its
schedule and is counted. The two columns are not comparable, and the
uncorrected figure says nothing about how late the edges were.

| SPI1 bursts (rx and tx) | Uncorrected: behind | Corrected (`comp`): behind |
|---|---|---|
| 16 (the driver's own) | 58-106 ms/s | 100 ms/s |
| 8 | 2.2 ms/s | |
| 4 | 0.014 ms/s | 3.1-4.1 ms/s |
| 2 | 0.014 ms/s | 3.3 ms/s |
| 1 | 0.014 ms/s | 3.1 ms/s |

- Burst length matters a great deal down to 4, and not below it: the corrected
  loop's remaining 3-4 ms/s (0.3-0.4 % slow, a twelfth of a semitone) is
  the same at 4, 2 and 1.
- It scales with the data rate, faster than linearly: 4.1, 1.6 and
  0.65 ms/s at card clocks of 25, 12.5 and 6.25 MHz (1.55, 1.06, 0.65 MB/s).
- It settles: a 40 s stream was 3.7 ms/s behind (148 ms in all), not
  growing. The loop just plays 0.37 % slow while the card is read.
- During a read the SPI1 interrupt fires about 25,000 times a second, 7.5
  per 512-byte sector, plus 2 DMA completions a sector (idle: none). The
  kernel's mmc-spi sends the command, polls for the data token, moves the
  data, then reads the CRC, as separate transfers, and only the data moves by
  DMA. So this is probably the CPU's register writes to the SPI controller
  (which the M0's pin write waits for, as with GPIO data writes), not DMA
  beats. Not checked.
- The driver's give-up check (`M0_COMP_BEHIND_US`, 1 ms/s) is made when a
  stream **ends**, over the whole stream, and then ignores `comp` for every
  later stream until `comp_gave_up` is written 0. So with the shipped
  settings the first stream played during a card read ends with
  `comp_gave_up` set, and the display redraw correction is gone for the rest
  of the session. A corrected loop 0.4 % slow is better than that, so the
  limit is now 20 ms/s (2 %): above the 3-9 ms/s the loop settles at during
  card reads, far below the ~100 ms/s it runs away at with 16-word bursts.
  Checked on the board with the module built against the image's kernel tree:
  three back-to-back 8 s streams during `dd` of the card (1.5 MB/s) ran
  corrected at 3.5-4.1 ms/s behind and `comp_gave_up` stayed N throughout.
- With `COMP_STATS=1` (7 fewer spare cycles) the same reads are 5.9-8.7
  ms/s behind, twice as bad: the loop is that sensitive to its spare cycles.
  Pin writes pushed later averaged 3.5-8 cycles a tick (burst 16 and 4), against 3.5 for
  the full-screen display redraw at 48 a second.
- The card's 50 MHz trial (`cap-sd-highspeed`) was not in effect: the
  card ran at 25 MHz in legacy timing.

**The register bus clock** (2026-10-11, kernel with the register-write cache,
`comp` on, 4-word bursts, card read at 1.6 MB/s). The SPI controllers' and
GPIO4's register bus, `pclk_lsperi_root`, runs at 93.75 MHz (the CRU's reset
value would be 187.5; `CRU_CLKSEL_CON29` at `0xFF9A0374`, bits 4:0 divide
`clk_gpll_div`, 375 MHz, bits 6:5 select the parent). Written live through
`/dev/mem` (the board stayed up; two 64 MB card reads gave identical sums):

| Divider | Register bus | Behind during the card read |
|---|---|---|
| /4 (as booted) | 93.75 MHz | 680-800 us/s |
| /3 | 125 MHz | 17-21 us/s (40 times less); 5 us/s with a console flood |

The board's device tree now asks for 187.5 MHz (`&cru` in the base dtsi). The
time behind turned out not to be what is heard: see "What the noise is".

**What the noise is** (2026-10-11, recorded at the headphone output with a
Tascam DR-series recorder, `-60 dBFS` with digital silence playing and nothing
else running; levels are total 20 Hz to 20 kHz, relative, not calibrated):

- Reading the SPI SD card gives broadband (white) noise at -22 dBFS (25 MHz
  card) to -14 dBFS (50 MHz) in the first recordings (-30 and -21 with the
  isolator, below); `comp` on or off, slow or fast pad slew, made no
  difference; reading the SDMMC card at 22 MB/s: -58 (nothing).
- **Correction (later the same day).** Those first recordings were made with the
  recorder's USB ground, the scope and the PicoCalc's supply all tied together.
  With a ground-loop isolator between the recorder and the PicoCalc and the
  PicoCalc on a clean charger, the picture changes: digital silence is -56 dBFS,
  SPI1 clocking all-ones with the card selected and idle (spidev, 25 MHz, burst
  4, 93.75 MHz register bus) is only -49, and 187.5 MHz with burst 1 is -51. So
  most of the "SPI1 clocking alone makes noise" result below was the measuring
  setup's ground loop, not the board. **Real card reads are still loud even with
  the isolator:** 25 MHz card, burst 4, 93.75 MHz bus: -30 dBFS; 50 MHz card,
  burst 4, 93.75 MHz bus: -21; 50 MHz, burst 2, 187.5 MHz bus: -29; burst 1:
  -30. The register-bus and burst settings still help real reads (8 dB at 50
  MHz), and the remaining -30 dBFS floor with a real read is not made by SPI1
  clocking: it needs the card reading (its supply and ground current on the
  mainboard are the suspects).
- Measured at the pins with a scope (Rigol DS1104Z+, 10x probes) the M0's output
  edges (timing against the 341.34 ns tick, edge counts, run lengths) and the
  pads' high level are the same with and without SPI1 load, at any burst length
  or bus clock: the noise is not visible at the SoC pins. The audio buffer
  (U501, NC7WZ16) is powered from AXP2101 ALDO4, whose input is the IPS/VSYS
  rail; a 1-bit DAC has no supply rejection, so ripple on that rail reaches the
  output while the pins toggle. VSYS carries 36 mV RMS (187.5 MHz register bus)
  to 108 mV RMS (93.75 MHz) in 20 Hz to 20 kHz on a clean charger, unrelated to
  card reads; 3V3_OUT is quiet (under 3 mV).
- The recordings below, made before the isolator, include the ground-loop
  contribution, so compare their settings with each other, not with the
  isolator numbers. SPI1 DMA burst length under the pure clocking load (25 MHz,
  93.75 MHz register bus unless noted), dBFS: 16: -17, 8: -22, 4: -26 (at 125
  MHz), 2: -31, 1: -41, 32-byte PIO transfers: -30. Register bus 93.75 / 125 /
  187.5 MHz at burst 4: -22.5 / -26 / -31. Real card reads at a 187.5 MHz
  register bus: 25 MHz card burst 4 -28 dBFS, burst 1 -38 (1.8 MB/s either way);
  50 MHz card burst 4 -21 (2.67 MB/s), burst 2 -35 (2.67 MB/s), burst 1 -34
  (1.98 MB/s). Before (93.75 MHz, burst 4): -22 at 25 MHz and -14 at 50 MHz.

**A loop-free baseline, and what it rules out** (2026-10-11, evening; Tascam
through a ground-loop isolator on the headphone jack, PicoCalc on battery with no
scope or meter attached, both channels checked with a 1 kHz tone at -11.6 dBFS;
left channel, total 20 Hz to 20 kHz, dBFS; the right channel is the same to
within 1 dB). Silence is -55.
Any extra ground connection (a scope probe ground, a meter lead) changes the
absolute numbers by an order of magnitude, so only compare within one session.

| SPI clock | DMA burst | register bus | noise | M0 lag |
|---|---|---|---|---|
| 25 MHz | 4 | 93.75 MHz | -28.9 (-21 in another run) | 2.5 ms/s |
| 25 MHz | 2 / 1 | 93.75 MHz | -31.3 / -31.7 | 1.8 / 1.4 ms/s |
| 50 MHz | 4 | 93.75 MHz | **-17.1** (`comp` gave up) | 90 ms/s |
| 50 MHz | 2 / 1 | 93.75 MHz | -30.0 / -31.9 | 7.6 / 27 ms/s |
| 25 MHz | 4 / 1 | 187.5 MHz | -21.2 / -30.6 | 2 / 4 us/s |
| 50 MHz | 4 / 2 / 1 | 187.5 MHz | -21.9 / -28.8 / -30.3 | 4 / 6 / 3 us/s |

- 4-word bursts are loud and erratic (-17 to -29 dBFS); 2- and 1-word bursts
  give a steady -29 to -32. That is up to 13 dB, and it was confirmed by ear on
  headphones (phases with 2-word bursts are clearly quieter; the noise that is
  left is still too noticeable against silence).
- A floor of about -30 dBFS (A-weighted -32) remains in every short-burst setting
  and barely moves with the card clock (12.5 MHz: -33; 25 MHz: -31; 50 MHz: -30)
  or the card (two cards). `comp` on or off makes no difference once the bursts
  are short (-29.3 off, -30.4 on, 25 MHz, burst 2, bus 187.5); at 4-word bursts
  `comp` off was 13 dB worse (-15.5 against -28.9).
- With `comp` off the noise rises smoothly with the M0 bit rate: 1.5 MHz -33.7,
  2 MHz -30.1, 3 MHz -29.3, 4 MHz -26.9 (about the number of edges per second).
- The noise in the left and right channels is uncorrelated as a waveform
  (correlation -0.02, coherence 0.006 in every band) but the loudness envelopes
  (10 ms) correlate at +0.9: both get loud and quiet at the same moments. An
  additive supply or ground disturbance would be correlated in the waveform
  (the idle reference's hum is: +0.57). So it is multiplicative: a common event
  in time acting on each channel's own independent bit stream.
- It is not data dependent (reading zeros and reading data are the same), not
  the ground return between the SD socket shield and the jack's ground lug (the
  DC differential moves by 4 to 5 uV; 50 mV DC range of an XDM1041), not a
  droop of the card's VDD (3.2965 V idle, 3.294 V reading), not a voltage step in
  the audio pad (drive 0 to 3, slew limit on or off make the ring after each edge
  nearly vanish or grow, and the noise stays at -30.2 to -30.4), not the
  M0's pin edges: scoped during real reads with `comp` on, the edge phases
  against the 341.34 ns tick are unchanged from idle (no edges in phases never
  seen at idle, 0.05 to 0.06 % in both). Measured at the SD socket's VDD pin
  (AC-coupled, full bandwidth, ambient FM pickup with the board off is
  0.3 mV in 20 kHz to 1 MHz): reading adds about 12 mV RMS in 20 kHz to 1 MHz
  (13.3 against 4.8 mV) and lines at the SPI clock and its harmonics (23.4375,
  46.875, 70.3 MHz, 20 to 27 dB above idle); the audio stream alone adds 15 to
  32 mV of ripple at 5 to 150 MHz. The buffer's own supply (ALDO4 at U501, input
  from IPS/VSYS) has not been probed: that is the open measurement.

A 10 uF + 100 nF ceramic pair soldered across the SD socket's VDD and VSS1 pads
(leads a few millimetres) changed the recorded noise by nothing (reads -30.3 before,
-30.5 after; references -50.8 and -50.7; same conditions, 25 MHz, burst 2, bus
187.5 MHz). It also did not remove the ripple it was meant to: the 20 kHz to 1 MHz
ripple at the VDD pin while reading went 13.3 -> 10.9 mV (idle 4.9 -> 1.8 mV, the
23.4375 MHz clock line 19.7 -> 14.1 dB above idle). So that test does not exclude
the card's supply: a card pulling tens of milliamps for tens of microseconds needs
a far larger reservoir, or a ferrite bead or a few ohms in the VDD feed, for the
ripple to drop by 20 dB. The decisive measurement remains the ripple on the
audio buffer's own supply (ALDO4 at U501).

Two more tests, with the scope's fourth channel tacked (fine wire) onto the
buffer's VCC pin (U501 pin 5, ALDO4):
- 47 pF ceramics from pin 31 and from pin 32 to ground (pin 33): the ring after each
  edge drops (median overshoot after a rise 2.25 -> 1.9 V, undershoot after a fall
  -0.47 -> -0.04 V), the tone level is unchanged (-11.6 dBFS), and the read noise is
  unchanged: -30.3 dBFS in every condition (before: -30.2 to -30.4). Ringing of the
  signal at the buffer's input, and its closeness to the switching threshold, is
  not the cause. (The idle reference rose from -50.8 to -44.4 dBFS, probably the
  added wire on VCC.)
- Supply ripple (mV RMS, AC-coupled, full bandwidth), 20 kHz-1 MHz / 1-5 / 5-30 /
  30-150 MHz. Buffer VCC: no stream, no read 0.30 / 0.50 / 2.1 / 5.7; stream, no read
  2.34 / 2.84 / 5.9 / 20.2; stream + read 25 MHz 2.39 / 2.88 / 5.8 / 19.5; stream +
  read 50 MHz 2.40 / 2.90 / 5.9 / 19.7; no stream + read 0.34 / 0.60 / 2.6 / 6.4. The
  card's VDD, at the same time: 2.6 -> 7.5 mV (25 MHz) and 8.5 mV (50 MHz) in
  20 kHz-1 MHz when reading. So reading ripples the card's supply but not the
  buffer's: the buffer's supply is not how the noise arrives.
What is left is interference picked up after the pins: the SPI1 traces, the socket
and the card itself are near the audio parts, and the SDMMC card (on the Lyra,
far from them) is silent. RF at the SPI clock and its harmonics, demodulated by each
channel's own stages, would give what was measured: independent per-channel
waveforms with a common envelope that follows the read bursts.

Where the noise is (scope, same cal tone and reads, AC-coupled, audio band):
at the jack tip the 1 kHz tone is 25 mV rms and silence is 0.17 / 0.20 / 0.27 / 0.40 mV
rms in 200-1k / 1-4k / 4-10k / 10-20k Hz; with the card read (25 MHz, 2-word
bursts) 0.54 / 1.05 / 1.42 / 1.58 mV, 50 MHz 0.61 / 1.21 / 1.65 / 1.84, 25 MHz with
a 93.75 MHz bus 0.63 / 1.23 / 1.70 / 1.88 (-20 dB re the tone, as the recorder
gave). The same excess is at the buffer's output (pin 6) and at its input (pin 31,
the M0's own pad): after scaling by the 1.83 V against 3.23 V swing it is 4.8 / 5.8 /
8.8 mV at pin 31 and 5.5 / 6.8 / 11 mV at pin 6. So the noise is already in the
M0's bitstream at the pin; the buffer and the output filter only pass it, and the
buffer's edges match the pin's (same edge count to 0.1 %, same share of narrow
pulses). Supply, ground, ring, threshold, missing pulses and pickup are excluded.
(Two-channel scope captures show spurious 64-sample mismatches between channels; do not
match edges across channels at this fidelity.)

The pin does not change when the M0's blocking write returns (timing diagnostic
firmware: pin 31 toggled every tick with the corrected loop's own write and SysTick
read, each count logged, the scope's edge times matched to the log tick by tick, 128-cycle
tick, silence). Pin edges land on a 10 MHz grid, 37.5 core cycles: each edge is at
`37.5 * ceil((R + x) / 37.5) + C`, R being the time the write returned by the SysTick
count (94 % of ticks within 6 cycles, rms 1.1 cycles = 3 ns, the rest are flips across a grid
step). x, the grid's phase against SysTick, is new at every stream start. The loop's
lateness measure said every write was on time; the pin edges are spread over 37.5
cycles (rms 10.7 cycles = 29 ns, the pattern repeating every 75 ticks since
128 mod 37.5 = 15.5). With a tick of 150 cycles (4 grid steps, 2.5 MHz) 89 % of the
intervals are within 4 cycles of 150 and the rest are +-37.5 steps. So the "write
returns when the pin changes" assumption of m0_play_comp is wrong by up to 37.5 cycles
(100 ns), and a tick that is a multiple of 37.5 cycles (75, 150, 225, 300, 375) makes
the delay the same every tick.

Measured with the card read (timing diagnostic firmware, 128-cycle tick, pin 31
toggled every tick, SysTick counted just before and just after the blocking pin
write and logged in the M0's own memory; the scope's edge times matched to the log
tick by tick, 3 x 939 ticks per condition):
- The pin edges are on the 10 MHz grid (37.5 cycles) in every condition, with the
  same coherence (0.94-0.96) and period, so a pin interval is a whole number of
  37.5-cycle steps (silence: 3 or 4; card read at 25 MHz: 3 or 4 on 98 % of ticks,
  5 to 7 on 1.3 %; at 50 MHz: 5 on 9 %).
- The pin follows when the write is issued, not when it returns. The count taken
  before the write predicts the pin interval to within one step on 96.8 / 87.6 /
  78 % of ticks (card read at 25 MHz with 2-word bursts / at 50 MHz / at 25 MHz with
  the 93.75 MHz bus and 4-word bursts); the count after the write, which
  m0_play_comp uses, on 84.6 / 74.0 / 63.2 %. Regressing the pin interval against
  the write's own blocking time gives a slope of +0.04 / -0.02 / -0.16: a stall
  inside the write does not move the pin.
- A write blocks for 53 cycles normally; with the card read the 99th percentile is
  101-105 and the longest 113 (27 % of ticks stalled at all, 9 % by more than 20
  cycles, at 25 MHz). The loop treats each stall as lateness and corrects for it.
  The rms difference between the loop's interval and the pin's is 37.5 / 37.9 /
  52.2 cycles with the post-write count, against 23.5 / 30.1 / 37.8 with the
  pre-write count: the post-write count adds 23-36 cycles rms (61-96 ns) of error to the
  correction (about 15 cycles rms of any interval is the grid step itself).
- Stalls before the write (the pre-write count 20 cycles or more late): 1.8 / 2.0 /
  12.3 % of ticks; the write returns after the next tick has begun (PENDSTSET) on
  0.6 / 1.2 / 8.8 %. Pin intervals that even the pre-write count does not explain
  (off by 1.5 steps or more): 2.3 / 9.9 / 15.4 % of ticks.

**What the "rain" is not** (by ear, same day, digital silence, comp on): the
noise that remains while the SPI SD card is read sounds like rain, gets
quieter at 12.5 and 6.25 MHz card clocks, louder with the console flooding, is
absent when the SDMMC card (`mmcblk0`, 22 MB/s, and throttled to 1.6 MB/s) is
read, and does not change with the audio pad's drive strength or slew
(`GPIO4_IOC_SARADC_CON` `0xFF4D8840`: drive 0 to 3, slew limit on or off). It
is therefore not the M0 being late (40 times less lag changed nothing audible)
and not storage reads in general. The SPI1 pins are already at 2 mA. Left: the
mainboard's SD slot (and the display) coupling into the audio buffer's supply
or input. Not measured: nobody has put a scope on the buffer's supply or input
during a card read.

**An idle card ticked once a second** (found 2026-10-09, with the corrected
loop, 375 MHz and four-word display bursts all in use). With a card in the
slot, mounted or not, the SPI1 interrupt fired every 1.014 s while the
display's controller and its DMA channels were silent, and a faint tick
could be heard at that rate in silence on headphones. Unbinding `mmc_spi`
from `spi1.0` stopped both. The cause: `mmc_spi` does not read the usual
`cd-gpios` property, only `gpios` (0: card detect, 1: write protect), so the
slot's card-detect pin was never requested (`cd polling` in the boot log)
and the MMC core sent the card a status command once a second to see
whether it was still there. The board file now names the pin `gpios`
(GPIO1_B2, low with a card in, pulled up on the mainboard: 0 with a card,
1 without, read from the pin), which gives the driver an interrupt and no
polling. Not yet booted. Reading or writing the card while audio plays is
still as above.

**Reading the card: the card clock, and burst length.** How far the M0
falls behind while 10 s of silence plays and the card is read with `dd`
(`comp` on, 375 MHz), with the card clock set through
`/sys/kernel/debug/mmc1/clock`:

| Card clock | Read speed | M0 behind |
|---|---|---|
| 25 MHz (as booted) | 1.7 MB/s | 26 ms/s (25-66) |
| 12.5 MHz | 1.1 MB/s | 16 ms/s |
| 6.25 MHz | 667 kB/s | 1 ms/s |
| no read | - | 0.002 ms/s |

Half the clock helps in proportion to the data; a quarter helps far more,
which is not understood. The data blocks do go by DMA (SPI1 has its
channels; transfers of 64 words or more use them), in bursts of 16 words
each way, and the working explanation is the display's: a burst cannot be
interrupted, and 16 words to a peripheral take about as long as a tick.
The bus priorities above make no difference, which fits. So the kernel
patch now also takes `rockchip,rx-dma-burst`, the board file asks for 4
words each way on SPI1, and both lengths can be changed while running
(`tx_dma_burst`, `rx_dma_burst` in `/sys/bus/platform/devices/ff130000.spi/`).
Not yet booted or measured: which lengths work, and whether short bursts
allow a faster card clock (`cap-sd-highspeed` for 50 MHz).

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
take twice as long or more (the DMA cannot keep the FIFO full; see the frame
rates under "Correcting for late pin writes"). The change is one line in the kernel
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
7. **Pops from late pin writes**: the display's SPI traffic, the SPI SD
   card and Linux's GPIO writes delay the M0's pin write. There is a
   correction ("Correcting for late pin writes") that works for the
   display given a 375 MHz core clock and four-word DMA bursts in the
   display's SPI driver. The m0-audio overlay as shipped turns on the first
   two; the bursts need the kernel patch. The SPI SD
   card is not dealt with.
8. **Bus mode** has not been run with `ring_sync` or with interpolation;
   the measurements are all TCM.
9. **Pop when a stream ends** (and presumably when it starts): the pins go
   from the 50% pattern of silence to low, a DC step through the output
   capacitor. Needs a ramp between the two.
10. **ALDO4 margin**, above.
