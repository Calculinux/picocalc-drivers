# recsess8.sh: real mmc_spi reads against the SPI1 DMA burst length. 7 conditions, 1..7 beeps.
D=/home/pico
if [ "$1" != run ]; then
	systemd-run --quiet --unit=recsess8-$$ sh "$D/recsess8.sh" run && echo "started: unit recsess8-$$"
	exit 0
fi
P=/sys/module/picocalc_snd_m0/parameters
CLK=/sys/kernel/debug/mmc1/clock
SPI1=/sys/bus/platform/devices/ff130000.spi
BAT=/sys/class/power_supply/picocalc
STAT=/sys/block/mmcblk1/stat
oclk=$(cat $CLK); RD=
restore() { python3 $D/lsperi.py 2 >/dev/null; kill $RD 2>/dev/null; pkill -x dd; echo 4 > $SPI1/tx_dma_burst; echo 4 > $SPI1/rx_dma_burst; echo $oclk > $CLK 2>/dev/null; echo Y > $P/comp; echo "restored"; }
trap restore EXIT
: > $D/recsess8.log
phase() { # n label clock burst read
	n=$1
	echo 0 > $P/comp_gave_up
	echo $4 > $SPI1/tx_dma_burst; echo $4 > $SPI1/rx_dma_burst
	echo $3 > $CLK 2>/dev/null
	echo "$(date +%T) condition $n ($n beeps): $2 [clock=$(cat $CLK) burst=$4 read=$5]" | tee -a $D/recsess8.log
	aplay -q -t raw -f S16_LE -r 48000 -c 2 $D/rec/q$n.raw &
	AP=$!
	sleep $(awk -v n=$n 'BEGIN{print 0.5+n*0.3+0.6}')
	s0=$(awk '{print $3}' $STAT); t0=$(cut -d' ' -f1 /proc/uptime)
	[ "$5" = 1 ] && { sh -c "while :; do dd if=/dev/mmcblk1 of=/dev/null bs=1M iflag=direct status=none; done" & RD=$!; }
	wait $AP
	s1=$(awk '{print $3}' $STAT); t1=$(cut -d' ' -f1 /proc/uptime)
	kill $RD 2>/dev/null; pkill -x dd; wait $RD 2>/dev/null; RD=
	echo "     $(awk -v a=$s0 -v b=$s1 -v t0=$t0 -v t1=$t1 'BEGIN{printf "%.2f MB/s", (b-a)*512/1048576/(t1-t0)}'), $(dmesg | grep 'M0 played' | tail -1 | sed 's/.*: //')" >> $D/recsess8.log
	sleep 2
}
echo "battery $(cat $BAT/capacity)% $(cat $BAT/status)  start $(date +%T)" >> $D/recsess8.log
sleep ${LEAD:-10}
python3 $D/lsperi.py 1 >> $D/recsess8.log
phase 1 "no read (reference), bus 187.5"          50000000 4 0
phase 2 "card read 25 MHz, burst 4, bus 187.5"    25000000 4 1
phase 3 "card read 25 MHz, burst 1, bus 187.5"    25000000 1 1
phase 4 "card read 50 MHz, burst 4, bus 187.5"    50000000 4 1
phase 5 "card read 50 MHz, burst 2, bus 187.5"    50000000 2 1
phase 6 "card read 50 MHz, burst 1, bus 187.5"    50000000 1 1
echo "$(date +%T) done" >> $D/recsess8.log
