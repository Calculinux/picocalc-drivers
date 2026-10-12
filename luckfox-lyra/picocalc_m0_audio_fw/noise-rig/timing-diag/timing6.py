import sys,time,json,numpy as np; sys.path.insert(0,'.')
import usbtmc
from sdboth_run import P
from jitter_run import board,SPI
from edges import edges
MEM="python3 - <<'X'\nimport mmap,os,struct,sys\nfd=os.open('/dev/mem',os.O_RDWR|os.O_SYNC)\nm=mmap.mmap(fd,4096,mmap.MAP_SHARED,mmap.PROT_READ|mmap.PROT_WRITE,offset=0xFFF83000)\n%s\nX"
ARM=MEM%"m[0x100:0x108]=struct.pack('<II',0,1)\nprint('armed')"
READ=MEM%"c=struct.unpack('<2I',m[0x100:0x108])\nprint(c[0],c[1])\nprint(m[0x110:0x110+1880].hex())"
BACK=28
def robust_edges(v,dt,lo_th,hi_th,refr):
    t,ri,_=edges(v,dt,lo_th=lo_th,hi_th=hi_th)
    keep=[]; last=-1.0
    for i in range(len(t)):
        if t[i]-last>=refr: keep.append(i); last=t[i]
    keep=np.array(keep,dtype=int)
    return t[keep],ri[keep]
def chk_scope(s):
    s.w(":RUN"); time.sleep(0.4)
    sr=float(s.q(":ACQuire:SRATe?")); md=float(s.q(":ACQuire:MDEPth?")); tb=float(s.q(":TIMebase:MAIN:SCALe?"))
    if abs(sr-1e9)>1e6 or md<12e6-1 or abs(tb-5e-4)>1e-9: raise RuntimeError("scope state wrong: srate %g mdepth %g tb %g"%(sr,md,tb))
def fetch(s,chan,max_points):
    s.w(f":WAVeform:SOURce CHANnel{chan}"); s.w(":WAVeform:MODE RAW"); s.w(":WAVeform:FORMat BYTE")
    def good(cmd,lim=1e6):
        for k in range(8):
            x=float(s.q(cmd))
            if abs(x)<lim: return x
            time.sleep(0.2)
        raise RuntimeError(cmd)
    xinc=good(":WAVeform:XINCrement?"); yinc=good(":WAVeform:YINCrement?",1e3); yorg=good(":WAVeform:YORigin?"); yref=good(":WAVeform:YREFerence?")
    mem=min(int(float(s.q(":ACQuire:MDEPth?"))),max_points)
    data=bytearray(); start=1; chunk=250000
    while start<=mem:
        stop=min(start+chunk-1,mem); s.w(f":WAVeform:STARt {start}"); s.w(f":WAVeform:STOP {stop}")
        data+=s.qb(":WAVeform:DATA?"); start=stop+1
    return (np.frombuffer(bytes(data),dtype=np.uint8).astype(np.float64)-yorg-yref)*yinc,xinc
def one(s,cycles):
    board("systemctl reset-failed 2>/dev/null; echo 0 > %s/comp_gave_up; systemd-run --quiet --unit=dg-%d aplay -q -t raw -f S16_LE -r 48000 -c 2 -d 30 /dev/zero"%(P,int(time.time()*10)%1000000))
    time.sleep(1.5)
    s.w(":SINGle")
    for i in range(50):
        if s.q(":TRIGger:STATus?") in ("WAIT","RUN","AUTO"): break
        time.sleep(0.05)
    sr=float(s.q(":ACQuire:SRATe?"))
    if abs(sr-1e9)>1e6:
        print("  scope sample rate %g, not 1e9: capture skipped"%sr,flush=True); s.w(":STOP"); return None
    board(ARM)
    t0=time.time()
    while s.q(":TRIGger:STATus?")!="STOP":
        if time.time()-t0>8: s.w(":STOP"); return None
        time.sleep(0.05)
    board("pkill -x aplay")
    v,dt=fetch(s,1,12000000)
    out=board(READ).split("\n")
    st=out[0].split(); hexs=out[1].strip()
    log=np.frombuffer(bytes.fromhex(hexs),dtype="<u2")[:940]
    if int(st[0])!=2: return None
    return v,dt,log
def analyse(v,dt,log,cycles):
    Tn=cycles/0.375e9
    t,ri=robust_edges(v,dt,0.5,1.2,200e-9)
    gap=np.diff(t); cand=np.flatnonzero((~ri[:-1])&(gap>10e-6))     # the log copy holds the pin low for ~36 us
    if not len(cand): return None
    mid=len(v)*dt/2; s=cand[np.argmin(np.abs(t[cand]-mid))]
    N=len(log); a=s-N
    if a<1 or ri[a]: return None
    E=t[a:s]*0.375e9/(1+28.9e-6)      # board cycles: scope clock runs +28.9 ppm fast against the M0 (measured with the 10 MHz reference)
    R=ri[a:s]
    pre=(log&0x7F).astype(np.int64); post=((log>>7)&0x7F).astype(np.int64); pend=((log>>14)&1).astype(np.int64)
    return E,R,pre,post,pend
def main():
    cycles=int(sys.argv[1]); conds=json.loads(sys.argv[2]); N=int(sys.argv[3]); tag=sys.argv[4]
    s=usbtmc.Scope(); s.w(":SYSTem:BEEPer OFF")
    for c in (2,3,4): s.w(f":CHANnel{c}:DISPlay 0")
    s.w(":CHANnel1:DISPlay 1"); s.w(":CHANnel1:PROBe 10"); s.w(":CHANnel1:SCALe 0.5"); s.w(":CHANnel1:OFFSet -0.9"); s.w(":CHANnel1:COUPling DC"); s.w(":CHANnel1:BWLimit OFF")
    s.w(":TRIGger:MODE EDGE"); s.w(":TRIGger:SWEep AUTO"); s.w(":RUN"); time.sleep(0.5)
    for attempt in range(5):
        s.w(":TIMebase:MAIN:SCALe 5e-4"); time.sleep(0.3); s.w(":ACQuire:MDEPth 12000000"); time.sleep(0.5)
        if abs(float(s.q(":ACQuire:SRATe?"))-1e9)<1e6: break
    chk_scope(s); time.sleep(0.3); print("scope depth",s.q(":ACQuire:MDEPth?"),"srate",s.q(":ACQuire:SRATe?"),flush=True)
    s.w(":TRIGger:MODE PULSe"); s.w(":TRIGger:PULSe:SOURce CHANnel1"); s.w(":TRIGger:PULSe:WHEN NGReater"); s.w(":TRIGger:PULSe:LWIDth %g"%(2.0*cycles/0.375e9)); s.w(":TRIGger:PULSe:LEVel 0.9"); s.w(":TRIGger:SWEep NORMal")
    hz=375000000/cycles
    try:
        for name,comp,clk,burst,bus,rd in conds:
            board("systemctl stop rdload scopestream 2>/dev/null; pkill -x aplay; pkill -x dd; echo %s > %s/comp; echo 0 > %s/comp_gave_up; echo %d > %s/tick_hz; python3 /home/pico/lsperi.py %s >/dev/null; echo %d > %s/tx_dma_burst; echo %d > %s/rx_dma_burst; echo %d > /sys/kernel/debug/mmc1/clock"%('Y' if comp else 'N',P,P,hz,P,bus,burst,SPI,burst,SPI,clk))
            time.sleep(1)
            if rd: board("systemd-run --quiet --unit=rdload sh -c 'while :; do dd if=/dev/mmcblk1 of=/dev/null bs=1M iflag=direct status=none; done'"); time.sleep(2)
            R=[]
            for k in range(N):
                r=one(s,cycles)
                if r is None: print("  capture %d failed"%k,flush=True); continue
                a=analyse(r[0],r[1],r[2],cycles)
                if a is None: print("  capture %d: no sync/parity"%k,flush=True); continue
                R.append(a)
            if not R: print(name,"no data"); continue
            np.savez_compressed("timing6_%s_%s.npz"%(tag,name.replace(" ","_").replace("+","p")),E=np.array([x[0] for x in R]),R=np.array([x[1] for x in R]),pre=np.array([x[2] for x in R]),post=np.array([x[3] for x in R]),pend=np.array([x[4] for x in R]),cycles=cycles)
            print("%-24s saved %d captures"%(name,len(R)),flush=True)
    finally:
        board("systemctl stop rdload scopestream 2>/dev/null; pkill -x dd; pkill -x aplay; python3 /home/pico/lsperi.py 3 >/dev/null; echo 4 > %s/tx_dma_burst; echo 4 > %s/rx_dma_burst; echo 25000000 > /sys/kernel/debug/mmc1/clock 2>/dev/null"%(SPI,SPI))
        s.w(":TRIGger:MODE EDGE"); s.w(":TRIGger:SWEep AUTO"); s.w(":RUN"); s.close()
if __name__=="__main__": main()
