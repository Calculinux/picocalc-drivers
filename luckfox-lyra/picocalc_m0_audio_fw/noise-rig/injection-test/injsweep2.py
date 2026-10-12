import sys,time,numpy as np; sys.path.insert(0,'.')
import injrun as R
def pt(comp,loops,mask,mode,label):
    R.stream(comp,loops,mask,mode)
    m,t=R.meas(2)
    print("%-34s comp %-3s K=%2d loops (%3d cyc) mask %-5s mode %d: bands %s total %.2f mV"%(label,"ON" if comp else "OFF",loops,4*loops,mask,mode,np.round(m[1:],2).tolist(),t),flush=True)
    return t
print("== A (resumed). delay sweep, 1 in 8 plain ticks, every write ==",flush=True)
for loops in (12,16,24,32):
    for comp in (0,1): pt(comp,loops,"7",0,"A")
print("== B. injection rate at K=8 loops (32 cycles) ==",flush=True)
for mask in ("1","7","31","127"):
    for comp in (0,1): pt(comp,8,mask,0,"B")
print("== C. edge selection at K=8 loops, 1 in 8 ==",flush=True)
for mode in (1,2):
    for comp in (0,1): pt(comp,8,"7",mode,"C")
R.board("pkill -x aplay"); R.s.w(":CHANnel4:COUPling DC"); R.s.w(":RUN"); R.s.close()
