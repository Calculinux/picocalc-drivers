import sys,time,json,numpy as np; sys.path.insert(0,'.')
import usbtmc
from capture import acquire
from sdboth_run import P
from jitter_run import board
from scipy import signal
s=usbtmc.Scope(); s.w(":SYSTem:BEEPer OFF")
for c in (1,2,3): s.w(f":CHANnel{c}:DISPlay 0")
s.w(":CHANnel4:PROBe 10"); s.w(":CHANnel4:BWLimit 20M"); s.w(":CHANnel4:OFFSet 0"); s.w(":CHANnel4:COUPling AC"); s.w(":CHANnel4:SCALe 0.01"); s.w(":CHANnel4:DISPlay 1")
def meas(n=2):
    out=[]
    for _ in range(n):
        for attempt in range(4):
            try:
                r=acquire(s,[4],depth=2400000,tb=4e-3,trig_ch=4,trig_level=0.0,timeout=10,max_points=2400000,sweep="AUTO"); break
            except RuntimeError:
                time.sleep(1.5); s.w(":STOP"); s.w(":RUN"); time.sleep(0.5)
        else: raise
        v=r[4]-r[4].mean(); fs=1/r["dt"]; f,p=signal.welch(v,fs,nperseg=1<<16); df=f[1]-f[0]
        b=lambda lo,hi: np.sqrt(p[(f>=lo)&(f<hi)].sum()*df)*1e3
        out.append([b(200,1000),b(1000,4000),b(4000,10000),b(10000,20000)])
    m=np.mean(out,axis=0); return m,float(np.sqrt((m**2).sum()))
def stream(comp,loops,mask,mode,read=False):
    hz=1464844 if comp else 1456311          # 256 cycles (corrected loop) or 258 (uncorrected loop)
    board("systemctl stop rdload 2>/dev/null; pkill -x dd; pkill -x aplay; systemctl reset-failed 2>/dev/null; echo N > %s/comp; echo 0 > %s/comp_gave_up; echo %d > %s/tick_hz"%(P,P,hz,P))
    board("python3 /home/pico/inject.py 0 0xFFFFFFFF 0")
    board("systemd-run --quiet --unit=ij-%d aplay -q -t raw -f S16_LE -r 48000 -c 2 -d 60 /dev/zero"%(int(time.time()*10)%1000000)); time.sleep(2.0)
    board("python3 /home/pico/inject.py %d %s %d"%(loops,mask,mode)); time.sleep(1.0)
if __name__=="__main__":
    for comp in (1,0):
        for loops in (0,16):
            stream(comp,loops,"7",0)
            m,t=meas(); print("comp %s, K=%d loops (%d cycles), 1 in 8 plain ticks: mV rms %s total %.2f"%("ON " if comp else "OFF",loops,4*loops,np.round(m,2).tolist(),t),flush=True)
            if loops: print("   dmesg:",board("dmesg | grep -E 'pin writes' | tail -1 | cut -c1-110").strip())
    board("pkill -x aplay; sleep 1; python3 /home/pico/inject.py count")
    print("injections counted at the end of the last stream:",board("python3 /home/pico/inject.py count").strip())
    s.w(":CHANnel4:COUPling DC"); s.w(":RUN"); s.close()
