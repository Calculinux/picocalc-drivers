import numpy as np
def edges_simple(v, dt, thr=None):
    lo,hi=np.percentile(v,[2,98])
    if thr is None: thr=(lo+hi)/2
    b=v>thr
    idx=np.flatnonzero(b[1:]!=b[:-1])
    # linear interpolation of the crossing
    v0=v[idx]; v1=v[idx+1]
    frac=(thr-v0)/(v1-v0)
    t=(idx+frac)*dt
    rising=b[idx+1]
    return t,rising,(lo,hi,thr)
def grid_residuals(t, T=None):
    # fit t_k = t0 + k*T, k=round((t-t0)/T); T from the median spacing / tick estimate
    d=np.diff(t)
    if T is None:
        T=341.3333e-9
    k=np.round((t-t[0])/T)
    A=np.vstack([np.ones_like(k),k]).T
    sol,res,_,_=np.linalg.lstsq(A,t,rcond=None)
    resid=t-A@sol
    return resid, sol[1], k

def edges(v, dt, lo_th=None, hi_th=None):
    """Schmitt-trigger edge finder with sub-sample interpolation of the mid-level crossing."""
    lo,hi=np.percentile(v,[10,90])
    mid=(lo+hi)/2
    if lo_th is None: lo_th=lo+0.25*(hi-lo)
    if hi_th is None: hi_th=lo+0.75*(hi-lo)
    state=np.zeros(len(v),dtype=bool)
    # vectorised schmitt: set where v>hi_th, clear where v<lo_th, hold otherwise
    s=np.full(len(v),-1,dtype=np.int8); s[v>hi_th]=1; s[v<lo_th]=0
    idx=np.flatnonzero(s>=0)
    st=s[idx]
    ch=np.flatnonzero(st[1:]!=st[:-1])        # transitions between definite states
    times=[];rising=[]
    for k in ch:
        i0=idx[k]; i1=idx[k+1]                # definite-low/high sample, then the other
        seg=v[i0:i1+1]
        up=st[k]==0
        # first crossing of mid within the segment
        cr=np.flatnonzero((seg[:-1]-mid)*(seg[1:]-mid)<=0)
        if len(cr)==0: continue
        j=cr[0]; a=seg[j]; b=seg[j+1]
        frac=0.0 if b==a else (mid-a)/(b-a)
        times.append((i0+j+frac)*dt); rising.append(up)
    return np.array(times),np.array(rising),(lo,hi,mid)
