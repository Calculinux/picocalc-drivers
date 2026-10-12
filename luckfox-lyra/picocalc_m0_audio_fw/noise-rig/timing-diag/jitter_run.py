import sys,time,subprocess,json
sys.path.insert(0,'.')
import usbtmc
from capture import acquire
from edges import edges, grid_residuals
import numpy as np

SSH=["ssh","-o","BatchMode=yes","-o","ConnectTimeout=6","root@192.168.6.110"]
def board(cmd):
    return subprocess.run(SSH+[cmd],capture_output=True,text=True,timeout=60).stdout.strip()
SPI="/sys/bus/platform/devices/ff130000.spi"
def to_spidev():
    board("systemctl stop blast 2>/dev/null; echo spi1.0 > /sys/bus/spi/drivers/mmc_spi/unbind 2>/dev/null; echo spidev > /sys/bus/spi/devices/spi1.0/driver_override; echo spi1.0 > /sys/bus/spi/drivers/spidev/bind 2>/dev/null; ls /dev/spidev1.0")
def to_mmc():
    board("systemctl stop blast 2>/dev/null; echo spi1.0 > /sys/bus/spi/drivers/spidev/unbind 2>/dev/null; echo > /sys/bus/spi/devices/spi1.0/driver_override; echo spi1.0 > /sys/bus/spi/drivers/mmc_spi/bind 2>/dev/null")
def setup(bus_div, burst, blast_hz):
    board("systemctl stop blast 2>/dev/null")
    board(f"python3 /home/pico/lsperi.py {bus_div} >/dev/null; echo {burst} > {SPI}/tx_dma_burst; echo {burst} > {SPI}/rx_dma_burst")
    if blast_hz:
        board(f"systemd-run --quiet --unit=blast python3 /home/pico/rec/blast.py {blast_hz} 4096")
    time.sleep(2.0)

from lock import lock_metrics
def measure(s, label, tb=5e-4, chans=(1,2)):
    r=acquire(s,list(chans),depth=12000000,tb=tb,trig_ch=1,trig_level=0.9,timeout=15,max_points=3000000)
    dt=r["dt"]; res=dict(label=label,dt_ns=dt*1e9)
    for c in chans:
        t,rising,_=edges(r[c],dt,lo_th=0.45,hi_th=1.35)
        m=lock_metrics(t*1e9); res["ch%d"%c]=m
    return res

if __name__=="__main__":
    s=usbtmc.Scope()
    s.w(":SYSTem:BEEPer OFF")
    for c in (1,2): s.w(f":CHANnel{c}:PROBe 10"); s.w(f":CHANnel{c}:SCALe 0.5"); s.w(f":CHANnel{c}:OFFSet -0.9")
    board("systemctl is-active scopestream || systemd-run --quiet --unit=scopestream aplay -q -t raw -f S16_LE -r 48000 -c 2 -d 1500 /dev/zero")
    conds=json.loads(sys.argv[1])
    to_spidev()
    out=[]
    try:
        for name,bus,burst,hz in conds:
            setup(bus,burst,hz)
            res=measure(s,name)
            a,b=res["ch1"],res["ch2"]
            print("%-30s CH1 std %5.2f p99 %6.2f out %.3f | CH2 std %5.2f p99 %6.2f out %.3f"%(name,a["std"],a["p99"],a["out_frac"],b["std"],b["p99"],b["out_frac"]),flush=True)
            out.append(res)
    finally:
        board("systemctl stop blast 2>/dev/null; python3 /home/pico/lsperi.py 3 >/dev/null; echo 4 > %s/tx_dma_burst; echo 4 > %s/rx_dma_burst"%(SPI,SPI))
        to_mmc()
        s.w(":RUN"); s.close()
    json.dump(out,open("jitter_results.json","w"),indent=1)
