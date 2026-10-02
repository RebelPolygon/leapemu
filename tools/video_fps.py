#!/usr/bin/env python3
"""Distinct-frame rate of a hardware capture segment: video_fps.py VIDEO T0 DUR [CROP]"""
import subprocess, sys, numpy as np
v, t0, dur = sys.argv[1], sys.argv[2], float(sys.argv[3])
crop = sys.argv[4] if len(sys.argv) > 4 else '446:436:93:20'
raw = subprocess.run(['ffmpeg', '-v', 'error', '-ss', t0, '-t', str(dur), '-i', v, '-vf',
                      f'crop={crop},scale=160:160:flags=area,format=gray', '-f', 'rawvideo', '-'], capture_output=True).stdout
F = np.frombuffer(raw, np.uint8).reshape(-1, 160 * 160).astype(np.float32)
d = np.abs(np.diff(F, axis=0)).mean(axis=1)
n = int((d > 0.8).sum())
print(f"t={t0}+{dur:.0f}s: {n / dur:.1f} distinct frames/s")
