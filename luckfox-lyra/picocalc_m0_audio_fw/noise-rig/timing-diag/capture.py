import sys,time
sys.path.insert(0,'.')
import numpy as np

def acquire(s, chans, depth=12000000, tb=1e-3, trig_ch=1, trig_level=0.9, timeout=20, max_points=None, sweep="NORMal"):
    for c in (1,2,3,4):
        want = c in chans
        s.w(f":CHANnel{c}:DISPlay {int(want)}")
    time.sleep(0.2)
    s.w(f":TIMebase:MAIN:SCALe {tb}")
    s.w(f":ACQuire:MDEPth {depth}")
    s.w(":TRIGger:MODE EDGE"); s.w(f":TRIGger:EDGE:SOURce CHAN{trig_ch}"); s.w(":TRIGger:EDGE:SLOPe POSitive")
    s.w(f":TRIGger:EDGE:LEVel {trig_level}"); s.w(f":TRIGger:SWEep {sweep}")
    time.sleep(0.2)
    s.w(":SINGle")
    t0=time.time()
    while True:
        st=s.q(":TRIGger:STATus?")
        if st=="STOP": break
        if time.time()-t0>timeout: s.w(":STOP"); raise RuntimeError("no trigger (status %s)"%st)
        time.sleep(0.05)
    out={}
    for c in chans:
        s.w(f":WAVeform:SOURce CHANnel{c}"); s.w(":WAVeform:MODE RAW"); s.w(":WAVeform:FORMat BYTE")
        s.w(":WAVeform:STARt 1"); s.w(":WAVeform:STOP 24000000")
        mem=int(float(s.q(":ACQuire:MDEPth?")))
        if max_points: mem=min(mem,max_points)
        def good(cmd,lim=1e6):
            for k in range(8):
                x=float(s.q(cmd))
                if abs(x)<lim: return x
                time.sleep(0.2)
            raise RuntimeError("bad answer for "+cmd)
        xinc=good(":WAVeform:XINCrement?"); yinc=good(":WAVeform:YINCrement?",1e3)
        yorg=good(":WAVeform:YORigin?"); yref=good(":WAVeform:YREFerence?")
        data=bytearray(); start=1; chunk=250000
        while start<=mem:
            stop=min(start+chunk-1,mem)
            s.w(f":WAVeform:STARt {start}"); s.w(f":WAVeform:STOP {stop}")
            data+=s.qb(":WAVeform:DATA?")
            start=stop+1
        out[c]=(np.frombuffer(bytes(data),dtype=np.uint8).astype(np.float64)-yorg-yref)*yinc
    out["dt"]=xinc
    return out
