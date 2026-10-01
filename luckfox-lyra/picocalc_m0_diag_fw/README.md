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
| 2 CAL | times fixed loops against TIMER0_CH4 | effective CPU speed (including fetch wait states); cycles per GPIO write, timer read, SRAM read |
| 3 PERIOD | 10,000 expiries with LOAD = 999 | whether the timer period is LOAD + 1 or LOAD counts |
| 4 POLL_LAT | 4096 ticks at 100 kHz, polling | min/max/mean expiry-to-detect latency when polling |
| 5 POLL_SLOW | 1000 ticks at 1 kHz, polling | toggles GPIO4_B2: 500 Hz square wave on pin 31 for one second |
| 6 WFI_SLOW | 1000 ticks at 1 kHz, WFI | whether WFI sleeps and is woken by the masked timer interrupt |
| 7 WFI_LAT | 4096 ticks at 100 kHz, WFI | min/max/mean wake latency with WFI |
| 8 RUN | 1 kHz WFI heartbeat, forever | absolute tick rate against Linux's clock; B2 keeps toggling |

Everything that might not work on this SoC (WFI) comes after everything that
needs only the timer, so stages 1-5 complete either way.

Stage 5 onward puts a 500 Hz square wave on pin 31. With ALDO4 set so the
NC7WZ16 accepts a 1.8 V high, that is an audible tone on the left channel,
which makes it a quick check of the pin, buffer and ALDO4 as well.

## Running it

Needs the `m0-audio` overlay applied (it creates the remoteproc device) and
nothing playing.

```sh
make                                   # rk3506-m0-diag.elf (arm-none-eabi-gcc)
make m0diag LINUX_CC=arm-linux-gnueabihf-gcc
# copy rk3506-m0-diag.elf to /lib/firmware/ and m0diag to the board, then:

cd /sys/class/remoteproc/remoteproc0
cat state                              # must be "offline"
echo rk3506-m0-diag.elf > firmware
echo start > state
sleep 3                                # stages 1-7 take about 2.5 s
m0diag

echo stop > state
echo rk3506-m0-audio.elf > firmware    # IMPORTANT: or the next playback boots the diagnostics
```

`m0diag -r` prints only the SoC registers and works with any firmware (or
none): whether the M0 is sleeping or locked up (`GRF_SOC_STATUS2`), and the
`GRF_PMU_MCU_ISO_CON*` bits that can block the M0 from peripherals.

`m0diag` needs `/dev/mem` (`CONFIG_DEVMEM`) and root.

## Reading the results

| You see | Meaning |
|---|---|
| "no diagnostic results" and `M0 locked up: YES` | the M0 faulted before or while writing the results block: wrong start address, image not at M0 address 0, or SRAM at `0xFFF89000` not writable by the M0 |
| "no diagnostic results", not locked up | the M0 is not running at all (reset not released, clock gated) |
| stuck at stage 2 or 3 | the timers are not counting: check the isolation bits and `CRU_GATE_CON06` |
| `HardFault during this stage` | the access that stage makes is blocked (GPIO4, CRU, TIMER0) |
| effective speed well below `hclk_m0` from `dmesg` | instruction fetches from SRAM cost wait states; scale the audio firmware's cycle estimates by the same ratio |
| a bus access costs much more than 2 cycles | that access is the expensive one in the audio loop |
| period = LOAD | the audio firmware's `DS_TIMER_LOAD` should be the period, not period - 1 |
| stage 6 never finishes, `M0 sleeping` near 100 % | WFI sleeps but the masked interrupt does not wake it: stay with polling |
| stage 6 finishes with a huge "no expiry" count, `M0 sleeping` 0 % | WFI returns immediately: it works only as a slow poll |
| stage 6 finishes with a small "no expiry" count | WFI works; compare the WFI latency spread with polling |
| WFI spread smaller than polling spread | build the audio firmware with `TICK_WFI=1` |
| heartbeat not 1000.00 ticks/s | `clk_gpll_div_100m` is not 100 MHz (e.g. 99 MHz from a 1188 MHz PLL); the audio tick rate is off by the same ratio |

Latency is the timer's own count read right after the expiry is detected, in
10 ns steps. The absolute value includes the fixed cost of getting to that
read; the **spread** (max - min) is the jitter the audio output would see.

## Emulator check

`make check-emu` (needs `pip install unicorn pyelftools`) runs the firmware
against a modelled timer and checks the results block. It validates the
firmware's logic only. WFI is a no-op in the emulator, so the WFI stages run
there as polling.
