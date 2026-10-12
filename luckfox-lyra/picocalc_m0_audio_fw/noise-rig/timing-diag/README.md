# Pin-timing diagnostic: what the M0's SysTick count says vs when the pin changes

Hand-off for whoever changes the lateness correction in `m0_play_comp` (play.S).
Measured 2026-10-12 on the Lyra with the PicoCalc SD card read over SPI1.

## Findings (128-cycle tick; numbers in docs/m0-audio.md, section after "What the noise is")

1. The GPIO4 pin does not change when the blocking write returns. Pin edges sit on a
   10 MHz grid (37.5 core cycles = 100 ns) in every condition: edge time =
   37.5 * ceil((issue_time + x) / 37.5) + C. x (the grid's phase against SysTick) is
   different at every stream start. A tick that is a multiple of 37.5 cycles
   (75, 150, 225, 300, 375) makes the extra delay the same every tick.
2. The pin follows when the write is ISSUED, not when it returns. The SysTick count
   read just before the `str` predicts the pin interval to within one grid step on
   96.8 / 87.6 / 78 % of ticks (card read at 25 MHz with 2-word DMA bursts / 50 MHz /
   25 MHz with a 93.75 MHz bus and 4-word bursts); the count after the write (what
   m0_play_comp uses) only 84.6 / 74.0 / 63.2 %. Regression of the pin interval on
   the write's own blocking stall: slope +0.04 / -0.02 / -0.16 (a stall inside the
   write does not move the pin).
3. The write blocks 53 cycles normally; under reads p99 = 101-105, max 113. The loop
   treats those stalls as lateness and corrects for it: rms error of its interval
   vs the pin's interval is 37.5 / 37.9 / 52.2 cycles, against 23.5 / 30.1 / 37.8 using
   the pre-write count (about 15 cycles rms of any interval is grid quantisation).
4. Lateness the pre-write count does show: 20+ cycles late on 1.8 / 2.0 / 12.3 % of
   ticks, PENDSTSET after the write on 0.6 / 1.2 / 8.8 %. Pin intervals that even the
   pre-write count does not explain (1.5+ grid steps): 2.3 / 9.9 / 15.4 % of ticks.
5. In silence the loop's measure is d = 0 on every tick but the pin jitters +-18.75
   cycles (rms 10.7 = 29 ns), repeating every 75 ticks (128 mod 37.5 = 15.5).
6. Not yet known: how to learn x at stream start (it is a property of the stream's
   start time against the 10 MHz clock); whether a pre-write SysTick read fits the
   production loop's spare cycles (17, 7 with COMP_STATS); what the stalls before
   the write and the 2-15 % unexplained intervals are.

## What is here

* `diag-*.patch`: the diagnostic firmware as diffs against this directory's parent
  (play.S, play.h, main.c, Makefile). Build with `make` (the Makefile patch adds
  `-DM0_DIAG`; `-DM0_DIAG_INJECT` adds a 40-cycle software delay on every 8th tick,
  which proved that log and scope edges line up). With it, any stream toggles pin B2
  (Lyra pin 31) every tick using the corrected loop's own write and SysTick read,
  logs for 940 ticks (SysTick count before the write: bits 6:0, after: bits 13:7,
  PENDSTSET after: bit 14) into a buffer in the M0's own memory (logging to system
  SRAM per tick perturbs what is measured), copies it to SRAM at M0_TRACE_ADDR + 0x10
  (0xFFF83110) after three low ticks (a long low pulse: the sync mark), sets
  `ctrl[0]` = 2 at 0xFFF83100 and waits for the host to write 1 to `ctrl[1]` (arm).
  128-cycle tick only (7-bit counts).
* `timing3.py` (+ `usbtmc.py`, `capture.py`, `edges.py`, `jitter_run.py`,
  `sdboth_run.py`): host side. Rigol DS1104Z+ over pyusb, pulse-width trigger on CH1
  (pin 31) at the sync mark, 6 M samples at 1 GS/s, per-tick comparison.
  `python3 timing3.py 128 '[["silence",1,25000000,2,187.5,0],["read 25MHz b2",1,25000000,2,187.5,1]]' 3 tag`
  (conditions: name, comp, SD clock, DMA burst, register bus MHz, read load).
  It saves `timing3_<tag>_<cond>.npz` (edge times in core cycles, parity, pre, post,
  pend); analysis used a 37.5-cycle grid fit (circular mean of the edge phases).

## Hardware / procedure caveats

* TCM mode loads the M0 firmware once per boot at the first stream: a test ELF needs
  `cp elf /lib/firmware/rk3506-m0-audio.elf` and a reboot BEFORE the first stream, and the
  stock ELF back (and another reboot) afterwards. `reboot` hangs this board
  (needs a power cycle); after a power cycle the SPI card may need
  `echo spi1.0 > /sys/bus/spi/drivers/mmc_spi/unbind; ... /bind`.
* The diagnostic firmware does not consume the audio ring, so the driver ends the
  stream after about 10 s: start a fresh `aplay` for each capture (timing3.py does).
* Scope: CH1 on pin 31, x10 probe, short ground, DC, 0.5 V/div, offset -0.9 V;
  comparing a 12M two-channel capture's edges across channels is unreliable (64-sample
  readout artefact), so the diagnostic uses one channel.
