# RK3506 M0 bring-up diagnostics

A separate M0 firmware (`rk3506-m0-diag.elf`) and a Linux tool (`m0diag`) that
measure, on a real board, what the audio firmware's timing depends on. It is
not part of the audio path; load it by hand, read the results, put the audio
firmware back.

## What it measures

Stages run in this order. The stage number is stored before each one starts,
so a hang or fault names the stage it happened in.

| Stage | What | Answers |
|---|---|---|
| 1 ALIVE | reached `main()` | M0 starts from `0xFFF88000` with the image linked at 0; it can write SRAM at `0xFFF89000` |
| 2 CAL | times fixed loops against TIMER0_CH4 | real cycles per instruction, per GPIO write, timer read, SRAM read, core-internal write |
| 3 PERIOD | 10,000 expiries with LOAD = 999 | whether the timer period is LOAD + 1 or LOAD counts |
| 4 POLL_LAT | 4096 ticks, polling | expiry-to-detect latency when polling, with histogram |
| 5 POLL_SLOW | 1000 slow ticks, polling | toggles GPIO4_B2: an audible tone on pin 31 for about a second |
| 6 WFI_SLOW | 1000 slow ticks, WFI | whether WFI sleeps and is woken by the masked timer interrupt; tone again |
| 7 WFI_LAT | 4096 ticks, WFI | latency as the timer's own counter reports it |
| 8 SYST_PROBE | start SysTick | whether the core has a SysTick timer; core clock rate |
| 9 WFI_STAMP | 4096 timer-woken WFI wakes | interval between wakes, stamped with SysTick: the wake jitter itself |
| 10 ACCESS | 4096 GPIO writes and timer reads | how many cycles one access takes, and whether that varies |
| 11 SYST_TICK | 4096 SysTick ticks | whether SysTick wakes WFI, and that wake's latency |
| 12 RUN | slow WFI heartbeat, forever | absolute timer rate against Linux's clock; pin quiet |

Everything that might not work on a given SoC comes after everything that
needs only the timer, and the SysTick stages cannot hang (a TIMER0 tick runs
alongside as a backstop).

Stages 5 and 6 put a square wave of about 470 Hz on pin 31, a second each.
That is the PicoCalc's left audio input, so it is loud; it is also the
quickest check that the pin, the NC7WZ16 buffer and its supply pass the
1.8 V signal.

## Running it

Nothing else may be using the M0 (remove the `m0-audio` overlay first if it
is applied). `m0-diag-overlay.dts` adds just the loader node, with the timer
channels' clock parent assigned so Linux selects and enables it. Without
that the timers have no clock and the firmware stops at stage 3.

```sh
make                                   # rk3506-m0-diag.elf (arm-none-eabi-gcc)
dtc -@ -I dts -O dtb -o m0-diag.dtbo m0-diag-overlay.dts
# copy rk3506-m0-diag.elf, m0-diag.dtbo, m0diag.c, diag.h and the
# rk3506_rproc.ko built for the running kernel to the board, e.g. /tmp/m0test

cd /tmp/m0test
gcc -O2 -o m0diag m0diag.c             # or cross-compile: make m0diag LINUX_CC=...
echo -n /tmp/m0test > /sys/module/firmware_class/parameters/path
mkdir /sys/kernel/config/device-tree/overlays/m0diag
cat m0-diag.dtbo > /sys/kernel/config/device-tree/overlays/m0diag/dtbo
insmod rk3506_rproc.ko                 # boots the firmware named in the overlay
sleep 5
./m0diag

echo stop  > /sys/class/remoteproc/remoteproc0/state     # and to run it again:
echo start > /sys/class/remoteproc/remoteproc0/state
```

To undo: `echo stop`, `rmmod rk3506_rproc`, `rmdir` the overlay directory,
and clear the firmware path (`echo -n > .../firmware_class/parameters/path`).

`m0diag -r` prints only the SoC registers and works with any firmware (or
none): whether the M0 is sleeping or locked up (`GRF_SOC_STATUS2`), and the
`GRF_PMU_MCU_ISO_CON*` bits that can block the M0 from peripherals.

`m0diag` needs `/dev/mem` (`CONFIG_DEVMEM`) and root. It takes the timer and
core clock rates from `/sys/kernel/debug/clk/clk_summary`, or from `-t HZ` and
`-c HZ`.

## Reading the results

| You see | Meaning |
|---|---|
| "no diagnostic results" and `M0 locked up: YES` | the M0 faulted before or while writing the results block: wrong start address, image not at M0 address 0, or SRAM at `0xFFF89000` not writable by the M0 |
| "no diagnostic results", not locked up | the M0 is not running at all (reset not released, clock gated) |
| "stopwatch did not count", stuck at stage 3 | the timer channels have no clock: their parent is gated (see the overlay's `assigned-clock-parents`) |
| `HardFault during this stage` | the access that stage makes is blocked (GPIO4, CRU, TIMER0) |
| straight-line instruction costs more than 1 cycle | the image is not running as TCM; scale cycle estimates accordingly |
| period = LOAD | timer loads should be the period, not period - 1 |
| stage 6 never finishes, `M0 sleeping` near 100 % | WFI sleeps but the masked interrupt does not wake it |
| stage 6 finishes with a huge "no expiry" count | WFI returns immediately: it works only as a slow poll |
| WFI_STAMP spread 0 | WFI wake is cycle-exact; any spread in WFI_LAT is the timer's counter readout, not the wake |
| ACCESS spread 0 | the bus adds no jitter to when a GPIO write lands |
| SysTick present, wakes WFI, constant latency | SysTick can be the tick, with no peripheral access per tick |
| "timer clock measured" differs from clk_summary | Linux's idea of the timer clock is wrong |

## Measured: Luckfox Lyra (RK3506G2), Calculinux 6.1.99, 2026-10-01

| | |
|---|---|
| `hclk_m0` (M0 core clock) | 187.5 MHz (GPLL 1500 MHz / 8) |
| `clk_gpll_div_100m` (timer clock) | 93.75 MHz, not 100 MHz; confirmed against Linux's clock |
| isolation registers | reset values, nothing blocked |
| image linked at 0, loaded at `0xFFF88000` | runs; stop / reload works |
| timer period | LOAD + 1 |
| straight-line instruction | 2.5 cycles (1 with no wait states) |
| two-instruction loop with a taken branch | 10 cycles (4) |
| GPIO4 DR write | 28 cycles extra in a loop; 33 start to finish, spread 0 |
| TIMER0 status read | 24 cycles extra in a loop; 30 start to finish, spread 0 |
| core-internal (NVIC) write | 2 cycles |
| SRAM read at `0xFFF89000` / through the low window | 6 / 2 cycles |
| polling, expiry to detect | spread 277 ns (52 cycles) |
| WFI woken by masked TIMER0_CH5 | works; NVIC line 19 pending on every wake; no spurious returns |
| WFI wake interval, SysTick-stamped | exactly 2000 cycles, 4096 of 4096: zero jitter |
| WFI latency as the timer reports it | spread 107 ns, uniform: an artefact of the timer's counter readout |
| SysTick | present, runs on the core clock, wakes WFI with its exception masked |
| SysTick-woken WFI latency | 3 cycles, constant |
| NC7WZ16 buffer with a 1.8 V input | passes it at the stock ALDO4 setting (loud tone) |

Consequences for the audio firmware: tick from SysTick and wait in WFI (no
jitter, and no peripheral access per tick apart from the GPIO write); budget
cycles with the measured costs, not the textbook ones.

## Emulator check

`make check-emu` (needs `pip install unicorn pyelftools`) runs the firmware
against a modelled timer and checks the results block. It validates the
firmware's logic only. WFI is a no-op in the emulator and SysTick is absent,
so the WFI stages run there as polling and the SysTick stages are skipped.
