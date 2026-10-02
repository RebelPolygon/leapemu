#!/usr/bin/env python3
"""Render spectrograms of one or more WAV files into a PNG (0-4 kHz).

usage: spectrogram.py OUT.png A.wav [B.wav ...]
Needs numpy, scipy and matplotlib.
"""
import sys, numpy as np, scipy.io.wavfile as w, scipy.signal as sg, matplotlib
matplotlib.use('Agg'); import matplotlib.pyplot as plt
files=sys.argv[2:]; out=sys.argv[1]
fig,ax=plt.subplots(len(files),1,figsize=(10,2.6*len(files)))
ax=np.atleast_1d(ax)
for a,f in zip(ax,files):
    sr,x=w.read(f); x=x.astype(float)
    if x.ndim>1: x=x.mean(1)
    f_,t,S=sg.spectrogram(x,sr,nperseg=256,noverlap=192)
    a.pcolormesh(t,f_,10*np.log10(S+1e-3),shading='auto',vmin=np.percentile(10*np.log10(S+1e-3),30))
    a.set_title(f"{f.split('/')[-1]} sr={sr} peak={np.abs(x).max():.0f} rms={np.sqrt((x**2).mean()):.0f}",fontsize=8)
    a.set_ylim(0,4000)
plt.tight_layout(); plt.savefig(out,dpi=70)
