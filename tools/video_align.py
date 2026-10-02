#!/usr/bin/env python3
"""Align a real-hardware capture against leapemu screenshots to measure speed.

video_align.py VIDEO T0 T1 CROP(w:h:x:y) EMU_GLOB EMU_FPS
Samples the video at 10 fps between T0 and T1 seconds, fingerprints both
sequences (32x32 normalised grey), aligns them with dynamic time warping and
fits emulator-time vs real-time over scene changes. Slope 1.0 = correct speed;
slope < 1 = emulator runs fast."""
import glob, struct, subprocess, sys, zlib
import numpy as np

video, t0, t1, crop, emu_glob, emu_fps = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), sys.argv[4], sys.argv[5], float(sys.argv[6])
vfps = float(sys.argv[7]) if len(sys.argv) > 7 else 10.0
S = 32
raw = subprocess.run(['ffmpeg', '-v', 'error', '-ss', str(t0), '-t', str(t1 - t0), '-i', video, '-vf',
                      f'fps={vfps},crop={crop},scale={S}:{S}:flags=area,format=gray', '-f', 'rawvideo', '-'],
                     capture_output=True).stdout
V = np.frombuffer(raw, np.uint8).reshape(-1, S * S).astype(np.float32)

def load_png(p):
    d = open(p, 'rb').read(); i = d.index(b'IDAT'); n = struct.unpack('>I', d[i - 4:i])[0]
    r = np.frombuffer(zlib.decompress(d[i + 4:i + 4 + n]), np.uint8).reshape(160, 1 + 160 * 3)[:, 1:].reshape(160, 160, 3)
    g = r.astype(np.float32) @ np.array([0.299, 0.587, 0.114], np.float32)
    return g.reshape(S, 160 // S, S, 160 // S).mean(axis=(1, 3)).ravel()

files = sorted(glob.glob(emu_glob))
E = np.stack([load_png(f) for f in files])

def norm(X):
    X = X - X.mean(axis=1, keepdims=True)
    return X / (X.std(axis=1, keepdims=True) + 1e-3)
Vn, En = norm(V), norm(E)
D = ((Vn[:, None, :] - En[None, :, :]) ** 2).mean(axis=2)  # N x M

# Subsequence DTW: the video segment may start/end anywhere in the emulator run.
N, M = D.shape
C = np.full((N, M), np.inf, np.float32)
C[0] = D[0]
for i in range(1, N):
    prev = C[i - 1]
    best = np.minimum(prev, np.concatenate(([np.inf], prev[:-1])))  # vertical / diagonal
    row = D[i] + best
    for j in range(1, M):  # horizontal steps
        if row[j - 1] + D[i, j] < row[j]: row[j] = row[j - 1] + D[i, j]
    C[i] = row
j = int(np.argmin(C[-1])); path = [(N - 1, j)]; i = N - 1
while i > 0:
    opts = [(C[i - 1, j], i - 1, j)]
    if j > 0: opts += [(C[i - 1, j - 1], i - 1, j - 1), (C[i, j - 1], i, j - 1)]
    _, i, j = min(opts); path.append((i, j))
path.reverse()

# Scene changes in the video = large frame-to-frame differences; use their matches.
dv = np.r_[0, np.abs(np.diff(Vn, axis=0)).mean(axis=1)]
cuts = [k for k in range(1, N) if dv[k] > np.percentile(dv, 90)]
first = {}
for vi, ej in path:
    first.setdefault(vi, ej)
pts = np.array([(vi / vfps, first[vi] / emu_fps) for vi in cuts if D[vi, first[vi]] < np.median(D.min(axis=1)) * 2])
slope, icpt = np.polyfit(pts[:, 0], pts[:, 1], 1)
resid = pts[:, 1] - (slope * pts[:, 0] + icpt)
print(f"video {t0:.0f}-{t1:.0f}s vs {len(files)} emulator frames: {len(pts)} scene-change anchors")
print(f"emulator seconds per real second: {slope:.3f}  (speed factor {1/slope:.3f}x real time), residual rms {resid.std():.3f}s")
print(f"video span {pts[0,0]:.1f}-{pts[-1,0]:.1f}s  ->  emulator span {pts[0,1]:.1f}-{pts[-1,1]:.1f}s")
