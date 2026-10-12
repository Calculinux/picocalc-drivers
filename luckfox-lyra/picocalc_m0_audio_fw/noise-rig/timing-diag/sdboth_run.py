import sys,time,json
sys.path.insert(0,'.')
import usbtmc
from capture import acquire
from jitter_run import board,SPI
import numpy as np
from scipy import signal
P="/sys/module/picocalc_snd_m0/parameters"
def clk(hz): board(f"echo {hz} > /sys/kernel/debug/mmc1/clock 2>/dev/null")
def cfg(stream,comp,clkhz,burst,bus,read):
    board("systemctl stop rdload scopestream 2>/dev/null; pkill -x dd; pkill -x aplay")
    board(f"echo {'Y' if comp else 'N'} > {P}/comp; echo 0 > {P}/comp_gave_up; python3 /home/pico/lsperi.py {bus} >/dev/null; echo {burst} > {SPI}/tx_dma_burst; echo {burst} > {SPI}/rx_dma_burst")
    clk(clkhz)
    if stream: board("systemd-run --quiet --unit=scopestream aplay -q -t raw -f S16_LE -r 48000 -c 2 -d 1500 /dev/zero"); time.sleep(2.0)
    if read: board("systemd-run --quiet --unit=rdload sh -c 'while :; do dd if=/dev/mmcblk1 of=/dev/null bs=1M iflag=direct status=none; done'"); time.sleep(2.0)
BANDS=[(2e4,1e6),(1e6,5e6),(5e6,30e6),(30e6,150e6)]
if __name__=="__main__":
    conds=json.loads(sys.argv[1]); s=usbtmc.Scope(); s.w(":SYSTem:BEEPer OFF")
    for c in (1,2): s.w(f":CHANnel{c}:DISPlay 0")
    for c in (3,4): s.w(f":CHANnel{c}:PROBe 10"); s.w(f":CHANnel{c}:COUPling AC"); s.w(f":CHANnel{c}:BWLimit OFF"); s.w(f":CHANnel{c}:SCALe 0.01"); s.w(f":CHANnel{c}:OFFSet 0"); s.w(f":CHANnel{c}:DISPlay 1")
    base=None; rows=[]
    try:
        for name,stream,comp,clkhz,burst,bus,read in conds:
            cfg(stream,comp,clkhz,burst,bus,read)
            psd={3:[],4:[]}
            for k in range(3):
                r=acquire(s,[3,4],depth=12000000,tb=1e-3,trig_ch=3,trig_level=0.0,timeout=10,max_points=6000000,sweep="AUTO")
                fs=1/r["dt"]
                for c in (3,4):
                    v=r[c]; f,p=signal.welch(v-v.mean(),fs,nperseg=1<<15); psd[c].append(p)
            df=f[1]-f[0]; out=[]
            for c,lab in ((3,"SD VDD"),(4,"BUFFER VCC")):
                p=np.mean(psd[c],axis=0); b=[np.sqrt(p[(f>=lo)&(f<hi)].sum()*df)*1e3 for lo,hi in BANDS]
                out.append("%s %s"%(lab," / ".join("%.2f"%x for x in b)))
                if c==4: rows.append((name,f,p))
            print("%-34s fs %.0f MS/s | mV RMS (20k-1M / 1-5M / 5-30M / 30-150M): %s"%(name,fs/1e6," | ".join(out)),flush=True)
        # which spectral lines grew most vs the first condition with stream+no read (index 1)
        ref=rows[1][2]; f=rows[1][1]
        for name,f_,p_ in rows[2:]:
            ratio=10*np.log10((p_+1e-20)/(ref+1e-20)); m=(f>3e5)&(f<1.0e8)
            idx=np.argsort(ratio[m])[-4:][::-1]
            print("   strongest growth vs stream-no-read, %-28s %s"%(name,[("%.3f MHz"%(f[m][i]/1e6),"%+.1f dB"%ratio[m][i]) for i in idx]))
        np.savez_compressed("sdvdd_psd.npz",**{n.replace(" ","_")[:30]:p for n,_,p in rows},f=rows[0][1])
    finally:
        board("systemctl stop rdload 2>/dev/null; pkill -x dd; echo Y > %s/comp; python3 /home/pico/lsperi.py 3 >/dev/null; echo 4 > %s/tx_dma_burst; echo 4 > %s/rx_dma_burst; echo 25000000 > /sys/kernel/debug/mmc1/clock 2>/dev/null"%(P,SPI,SPI))
        s.w(":CHANnel3:COUPling DC"); s.w(":CHANnel4:COUPling DC"); s.w(":RUN"); s.close()
