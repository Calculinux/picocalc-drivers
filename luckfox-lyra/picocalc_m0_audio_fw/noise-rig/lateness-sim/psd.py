import numpy as np,sys
from scipy import signal
a=np.fromfile("area.f32",dtype=np.float32).astype(np.float64)
fs=2.9296875e6
f,p=signal.welch(a-a.mean(),fs,nperseg=1<<17)
df=f[1]-f[0]
def band(lo,hi): return np.sqrt(p[(f>=lo)&(f<hi)].sum()*df)
tot=np.sqrt(p[(f>=20)&(f<20000)].sum()*df)
# dB re a full-scale sine (rms 0.7071)
print("%7.1f dB   (1-4k %.1f  4-10k %.1f  10-20k %.1f)"%(20*np.log10(tot/0.7071),20*np.log10(band(1000,4000)/0.7071),20*np.log10(band(4000,10000)/0.7071),20*np.log10(band(10000,20000)/0.7071)))
