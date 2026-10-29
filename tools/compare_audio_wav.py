#!/usr/bin/env python3
"""Compare two recordings of the same scene (the host build's exact SPU output, --audio, against a DS capture,
tools/ds_audio_capture.sh): the tempo ratio and offset that align their loudness envelopes, and the DS's spectrum
relative to the host's in 24 log-spaced bands (levels normalised).

    python3 tools/compare_audio_wav.py HOST.wav DS.wav
"""
import sys, wave, numpy as np
def load(p):
    w=wave.open(p); d=np.frombuffer(w.readframes(w.getnframes()),dtype=np.int16).reshape(-1,2).astype(float).mean(1); return d, w.getframerate()
def env(x,r,hop=0.02):
    s=int(r*hop); n=len(x)//s; return np.sqrt((x[:n*s].reshape(n,s)**2).mean(1)), hop
def spec(x,r,t0,dur,nfft=2048):
    seg=x[int(t0*r):int((t0+dur)*r)]; hop=nfft//2
    frames=[np.abs(np.fft.rfft(seg[i:i+nfft]*np.hanning(nfft))) for i in range(0,len(seg)-nfft,hop)]
    S=np.mean(frames,0); f=np.fft.rfftfreq(nfft,1/r)
    bands=np.logspace(np.log10(60),np.log10(16000),25)
    return np.array([S[(f>=bands[i])&(f<bands[i+1])].mean() for i in range(24)]), bands
h,hr=load(sys.argv[1]); d,dr=load(sys.argv[2])
eh,hop=env(h,hr); ed,_=env(d,dr)
eh/=eh.max()+1e-9; ed/=ed.max()+1e-9
best=None
for ratio in np.arange(0.90,1.101,0.005):  # ds time = host time * ratio
    idx=np.arange(len(eh))*ratio
    for off in range(0,len(ed)-50):
        j=(idx+off).astype(int); ok=j<len(ed)
        if ok.sum()<len(eh)*0.6: break
        a=eh[ok]; b=ed[j[ok]]
        if a.std()<1e-6 or b.std()<1e-6: continue
        c=np.corrcoef(a,b)[0,1]
        if best is None or c>best[0]: best=(c,ratio,off*hop)
print(f"tempo ratio ds/host {best[1]:.3f}, offset {best[2]:.2f} s, envelope correlation {best[0]:.3f}")
# spectra over the shared music
t_host=np.nonzero(eh>0.05)[0][0]*hop; dur=min(len(h)/hr-t_host, (len(d)/dr-best[2]-t_host*best[1]))-0.5
sh,b=spec(h,hr,t_host,dur); sd,_=spec(d,dr,best[2]+t_host*best[1],dur)
sh=20*np.log10(sh/sh.sum()+1e-12); sd=20*np.log10(sd/sd.sum()+1e-12)
diff=sd-sh
print('band  host-relative dB (ds - host), normalised levels:')
print(' '.join(f"{int(b[i]):>5}:{diff[i]:+.1f}" for i in range(24)))
print(f"overall level ratio ds/host {np.sqrt((d**2).mean())/np.sqrt((h**2).mean()):.2f}")
