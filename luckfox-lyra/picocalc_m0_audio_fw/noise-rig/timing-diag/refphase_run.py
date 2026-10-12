import sys,time,json,numpy as np; sys.path.insert(0,'.')
import usbtmc
from capture import acquire
from sdboth_run import P
from jitter_run import board,SPI
from edges import edges
def ref_edges(v,dt):
    t,ri,_=edges(v,dt,lo_th=0.9,hi_th=2.4)
    return t[ri]                     # rising crossings (mid level), seconds
def phase_stats(tp,tr,label):
    k=np.searchsorted(tr,tp)-1
    ok=(k>=0)&(k<len(tr)-1)
    u=(tp[ok]-tr[k[ok]])*1e9          # ns after the previous reference rising edge
    per=np.diff(tr)*1e9
    c=abs(np.mean(np.exp(2j*np.pi*u/100.0)))
    h,_=np.histogram(u,bins=np.arange(0,101,10))
    print("   %-28s edges %6d | ref period mean %.4f ns std %.3f ns | coherence of pin-edge position in the 100 ns cycle: %.3f | hist per 10 ns: %s"%(label,len(u),per.mean(),per.std(),c,(100*h/h.sum()).round(0).astype(int).tolist()),flush=True)
    return u
def main():
    conds=json.loads(sys.argv[1]); N=int(sys.argv[2]); tag=sys.argv[3]
    s=usbtmc.Scope(); s.w(":SYSTem:BEEPer OFF")
    for c in (2,4): s.w(f":CHANnel{c}:DISPlay 0")
    s.w(":CHANnel1:PROBe 10"); s.w(":CHANnel1:SCALe 0.5"); s.w(":CHANnel1:OFFSet -0.9"); s.w(":CHANnel1:COUPling DC"); s.w(":CHANnel1:BWLimit OFF")
    s.w(":CHANnel3:PROBe 1"); s.w(":CHANnel3:SCALe 1.0"); s.w(":CHANnel3:OFFSet -1.7"); s.w(":CHANnel3:COUPling DC"); s.w(":CHANnel3:BWLimit OFF")
    s.w(":TRIGger:MODE EDGE")
    try:
        for name,clk,burst,bus,rd,comp in conds:
            board("systemctl stop rdload 2>/dev/null; pkill -x dd; pkill -x aplay; echo %s > %s/comp; echo 0 > %s/comp_gave_up; python3 /home/pico/lsperi.py %s >/dev/null; echo %d > %s/tx_dma_burst; echo %d > %s/rx_dma_burst; echo %d > /sys/kernel/debug/mmc1/clock"%('Y' if comp else 'N',P,P,bus,burst,SPI,burst,SPI,clk))
            board("systemd-run --quiet --unit=rp-%d aplay -q -t raw -f S16_LE -r 48000 -c 2 -d 600 /dev/zero"%(int(time.time())%100000)); time.sleep(2)
            if rd: board("systemd-run --quiet --unit=rdload sh -c 'while :; do dd if=/dev/mmcblk1 of=/dev/null bs=1M iflag=direct status=none; done'"); time.sleep(2)
            print(name,flush=True)
            for k in range(N):
                if "DIAG" in name: board("pkill -x aplay; systemctl reset-failed 2>/dev/null; echo 0 > %s/comp_gave_up; systemd-run --quiet --unit=rq-%d aplay -q -t raw -f S16_LE -r 48000 -c 2 -d 30 /dev/zero"%(P,int(time.time()*10)%1000000)); time.sleep(1.5)
                r=acquire(s,[1,3],depth=int(sys.argv[4]),tb=float(sys.argv[5]),trig_ch=3,trig_level=1.65,timeout=10,max_points=int(sys.argv[4]),sweep="AUTO")
                dt=r["dt"]; tp,rp,_=edges(r[1],dt,lo_th=0.45,hi_th=1.35); tr=ref_edges(r[3],dt)
                phase_stats(tp,tr,"pin 31 all edges (dt %.0f ns)"%(dt*1e9))
                phase_stats(tp[rp],tr,"  rising only"); phase_stats(tp[~rp],tr,"  falling only")
    finally:
        board("systemctl stop rdload 2>/dev/null; pkill -x dd; pkill -x aplay; echo Y > %s/comp; python3 /home/pico/lsperi.py 3 >/dev/null; echo 4 > %s/tx_dma_burst; echo 4 > %s/rx_dma_burst; echo 25000000 > /sys/kernel/debug/mmc1/clock 2>/dev/null"%(P,SPI,SPI))
        s.w(":CHANnel3:PROBe 10"); s.w(":RUN"); s.close()
if __name__=="__main__": main()
