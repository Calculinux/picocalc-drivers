#!/bin/sh
# What does reading the SD card (SPI1, mmc-spi) do to M0 audio?
#
# Run as root on the PicoCalc, with nothing else playing:
#     sh sd-audio-test.sh
#
# Plays digital silence for DUR seconds through ALSA while reading the raw
# card with dd, once per SPI1 DMA burst length, and prints the driver's
# "M0 played ... behind" figure for each stream. Baselines with no card read
# come first and last. The card must not be mounted. Burst lengths (and the
# card clock, if CLOCKS is given) are put back when it ends.
#
#   DUR=15            seconds per stream
#   BURSTS="16 8 4 2 1"   SPI1 TX+RX burst lengths to try (16 = the stock driver)
#   CLOCKS="50000000 25000000"  card clocks to repeat the whole sweep at
#                     (/sys/kernel/debug/mmc1/clock); default: leave as is
#   COMP=0|1          force the corrected loop off or on for the run (default: as is)
#   DEV=/dev/mmcblk1  raw card device
set -u

SPI1=/sys/bus/platform/devices/ff130000.spi
CLK=/sys/kernel/debug/mmc1/clock
GAVE=/sys/module/picocalc_snd_m0/parameters/comp_gave_up
COMPP=/sys/module/picocalc_snd_m0/parameters/comp
COMP=${COMP:-}
DUR=${DUR:-15}
BURSTS=${BURSTS:-"16 8 4 2 1"}
CLOCKS=${CLOCKS:-}
DEV=${DEV:-/dev/mmcblk1}
STAT=/sys/block/$(basename "$DEV")/stat

die() { echo "error: $*" >&2; exit 1; }

[ "$(id -u)" = 0 ] || die "run as root"
[ -w "$SPI1/rx_dma_burst" ] || die "$SPI1/rx_dma_burst missing: the kernel does not have the burst sysfs files"
[ -r "$DEV" ] || die "$DEV not readable"
[ -r "$STAT" ] || die "$STAT not readable"
grep -q " $DEV" /proc/mounts && die "$DEV or a partition is mounted: unmount it first"
pgrep -x ffmpeg >/dev/null && die "ffmpeg is running: stop it first"
pgrep -x aplay >/dev/null && die "aplay is running"

orig_rx=$(cat "$SPI1/rx_dma_burst")
orig_tx=$(cat "$SPI1/tx_dma_burst")
orig_clk=
[ -r "$CLK" ] && orig_clk=$(cat "$CLK")
DD_PID=
orig_comp=$(cat "$COMPP")
if [ -n "$COMP" ]; then [ "$COMP" = 1 ] && echo Y > "$COMPP" || echo N > "$COMPP"; fi

restore() {
    echo "$orig_comp" > "$COMPP"
    [ -n "$DD_PID" ] && kill "$DD_PID" 2>/dev/null
    echo "$orig_rx" > "$SPI1/rx_dma_burst"
    echo "$orig_tx" > "$SPI1/tx_dma_burst"
    [ -w "$GAVE" ] && echo 0 > "$GAVE"
    [ -n "$CLOCKS" ] && [ -n "$orig_clk" ] && echo "$orig_clk" > "$CLK" 2>/dev/null
}
trap restore EXIT INT TERM

now() { awk '{print $1}' /proc/uptime; }
sectors() { awk '{print $3}' "$STAT"; }
played_lines() { dmesg | grep -c 'M0 played'; }

# run_case LABEL BURST READ(0|1)
run_case() {
    label=$1 burst=$2 read=$3
    [ -w "$GAVE" ] && echo 0 > "$GAVE"   # comp stays off for the session once it gave up
    [ -n "$burst" ] && { echo "$burst" > "$SPI1/rx_dma_burst"; echo "$burst" > "$SPI1/tx_dma_burst"; }
    n0=$(played_lines)
    DD_PID=
    if [ "$read" = 1 ]; then
        dd if="$DEV" of=/dev/null bs=1M iflag=direct status=none &
        DD_PID=$!
        sleep 1
    fi
    s0=$(sectors); t0=$(now)
    aplay -q -D default -t raw -f S16_LE -r 48000 -c 2 -d "$DUR" /dev/zero
    rc=$?
    s1=$(sectors); t1=$(now)
    if [ -n "$DD_PID" ]; then kill "$DD_PID" 2>/dev/null; wait "$DD_PID" 2>/dev/null; DD_PID=; fi
    sleep 1
    mbs=$(awk -v a="$s0" -v b="$s1" -v t0="$t0" -v t1="$t1" 'BEGIN{printf "%.2f", (b-a)*512/1048576/(t1-t0)}')
    line=
    [ "$(played_lines)" -gt "$n0" ] && line=$(dmesg | grep 'M0 played' | tail -1)
    behind=$(echo "$line" | sed -n 's/.*(\([0-9]*\) us per second).*/\1/p')
    total=$(echo "$line" | sed -n 's/.*: \([0-9]*\) us behind.*/\1/p')
    late=$(dmesg | grep 'pin writes:' | tail -1 | sed -n 's/.*, \([0-9]*\) per 1000 ticks.*/\1/p')
    [ "$rc" = 0 ] || behind="aplay rc=$rc"
    gave=; [ "$(cat "$GAVE" 2>/dev/null)" = Y ] && gave="  [comp gave up]"
    [ -n "$behind" ] || behind="no driver line"
    printf '%-28s %8s MB/s   behind %8s us total, %s us/s, late %s/1000 ticks%s\n' "$label" "$mbs" "${total:--}" "$behind" "${late:--}" "$gave"
}

sweep() {
    run_case "no card read (first)"   "$orig_rx" 0
    for b in $BURSTS; do
        run_case "card read, bursts $b" "$b" 1
    done
    run_case "no card read (last)"    "$orig_rx" 0
}

echo "comp=$(cat "$COMPP") tick_hz=$(cat /sys/module/picocalc_snd_m0/parameters/tick_hz)  $DUR s per stream, device $DEV"
echo "SPI1 bursts at start: rx=$orig_rx tx=$orig_tx  card clock: ${orig_clk:-unknown}"
echo

if [ -z "$CLOCKS" ]; then
    sweep
else
    for c in $CLOCKS; do
        echo "$c" > "$CLK" || die "cannot set card clock"
        echo "--- card clock $c Hz (reads back: $(cat "$CLK" | tr '\n' ' '))"
        sweep
    done
fi
