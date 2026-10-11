#!/usr/bin/env python3
"""Analyze a recsess recording: find the 1 kHz beep groups (n beeps = condition n),
then report the noise in the 12 s of silence after each group.

usage: analyze_rec.py RECORDING.wav [outdir]
Levels are relative: dB re the recording's full scale, so compare conditions with each other,
not with absolute numbers (the line-in gain is unknown).
"""
import sys, os, subprocess, tempfile
import numpy as np
from scipy import signal
from scipy.io import wavfile

src = sys.argv[1]
outdir = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(src)[0] + "_analysis"
os.makedirs(outdir, exist_ok=True)

# Normalise whatever we were given to mono float at its own rate via ffmpeg -> wav (int32)
tmp = tempfile.mkdtemp()
wavp = os.path.join(tmp, "in.wav")
subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", src, "-ac", "1", "-c:a", "pcm_s32le", wavp], check=True)
fs, x = wavfile.read(wavp)
x = x.astype(np.float64) / 2**31
print(f"{src}: {len(x)/fs:.1f} s at {fs} Hz, peak {np.abs(x).max():.3f}")

# --- beep detection: energy in a narrow band around 1 kHz, short windows
b, a = signal.butter(4, [900, 1100], btype="band", fs=fs)
env = np.abs(signal.hilbert(signal.filtfilt(b, a, x)))
win = int(0.02 * fs)
env_s = np.convolve(env, np.ones(win) / win, mode="same")
thr = 0.5 * np.percentile(env_s, 99.9)
on = env_s > thr
edges = np.flatnonzero(np.diff(on.astype(int)) == 1)
# keep only edges separated by >= 0.1 s (one per beep)
beeps = []
for e in edges:
    if not beeps or e - beeps[-1] > 0.1 * fs:
        beeps.append(e)
beeps = np.array(beeps) / fs
# group beeps: gap < 0.6 s belongs to the same group
groups = []
for t in beeps:
    if groups and t - groups[-1][-1] < 0.6:
        groups[-1].append(t)
    else:
        groups.append([t])
print(f"{len(beeps)} beeps in {len(groups)} groups: " + ", ".join(str(len(g)) for g in groups))

bands = [(20, 200), (200, 1000), (1000, 4000), (4000, 10000), (10000, 20000)]
def a_weight(f):
    f2 = f**2
    ra = (12194.0**2 * f2**2) / ((f2 + 20.6**2) * np.sqrt((f2 + 107.7**2) * (f2 + 737.9**2)) * (f2 + 12194.0**2))
    return ra / 0.7943  # ~0 dB at 1 kHz (A(1k) = 1.0000 after the usual +2.0 dB offset)

rows = []
specs = {}
for g in groups:
    n = len(g)
    t0 = g[-1] + 0.15 + 0.6          # after the last beep and the start of the load
    seg = x[int(t0 * fs): int((t0 + 10.0) * fs)]
    if len(seg) < 5 * fs:
        continue
    seg = seg - seg.mean()
    f, p = signal.welch(seg, fs, nperseg=1 << 15)
    df = f[1] - f[0]
    tot = np.sqrt(np.sum(p[(f >= 20) & (f <= 20000)]) * df)
    pa = p * a_weight(np.maximum(f, 1.0)) ** 2
    atot = np.sqrt(np.sum(pa[(f >= 20) & (f <= 20000)]) * df)
    bl = [np.sqrt(np.sum(p[(f >= lo) & (f < hi)]) * df) for lo, hi in bands]
    # crest/gaps: how bursty is it? short-term RMS (10 ms) 5th and 95th percentile
    w = int(0.01 * fs)
    r = np.sqrt(np.convolve(seg**2, np.ones(w) / w, mode="valid")[::w])
    p5, p95 = np.percentile(r, [5, 95])
    rows.append((n, t0, tot, atot, bl, p5, p95))
    specs[n] = (f, p)

db = lambda v: 20 * np.log10(max(v, 1e-12))
hdr = "cond  t0(s)  total  A-wtd |" + "".join(f" {lo}-{hi}Hz".rjust(14) for lo, hi in bands) + " | 10ms-rms p5 / p95"
print(hdr)
for n, t0, tot, atot, bl, p5, p95 in rows:
    print(f"{n:>4} {t0:7.1f} {db(tot):6.1f} {db(atot):6.1f} |" + "".join(f"{db(v):14.1f}" for v in bl) + f" | {db(p5):6.1f} / {db(p95):6.1f}")

# spectra plot
try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    fig, ax = plt.subplots(figsize=(10, 6))
    for n, (f, p) in sorted(specs.items()):
        m = (f >= 20) & (f <= 20000)
        ax.semilogx(f[m], 10 * np.log10(p[m] + 1e-30), label=f"cond {n}", lw=1)
    ax.set_xlabel("Hz"); ax.set_ylabel("dB/Hz re FS"); ax.grid(True, which="both", alpha=.3); ax.legend(ncol=2)
    fig.savefig(os.path.join(outdir, "spectra.png"), dpi=110)
    print("wrote", os.path.join(outdir, "spectra.png"))
except Exception as e:
    print("no plot:", e)
