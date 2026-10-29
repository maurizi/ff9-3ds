#!/usr/bin/env python3
"""Compare two recordings of the same scene (the host build's exact SPU output, --audio, against a DS capture,
tools/ds_audio_capture.sh): the tempo ratio and offset that align their loudness envelopes, and the DS's spectrum
relative to the host's in 24 log-spaced bands (levels normalised).

melonDS repeats its last output sample when its audio buffer runs dry (an underrun on this machine, not the port's
output): every such hold is a step, a thump of low-frequency energy (+15..+20 dB at 60-180 Hz over a d3_f2305
capture). The spectra leave out the windows touching a hold (12 samples or more of the same nonzero stereo value)
and the count is printed.

    python3 tools/compare_audio_wav.py HOST.wav DS.wav
"""
import sys, wave, numpy as np
def load(p):
    w=wave.open(p); x=np.frombuffer(w.readframes(w.getnframes()),dtype=np.int16).reshape(-1,2); r=w.getframerate()
    same=np.all(x[1:]==x[:-1],axis=1)&np.any(x[1:]!=0,axis=1)
    e=np.diff(np.concatenate([[0],same.astype(np.int8),[0]])); st=np.nonzero(e==1)[0]; en=np.nonzero(e==-1)[0]
    held=np.zeros(len(x),bool); pad=int(r*0.004); n=0
    for a,b in zip(st,en):
        if b-a>=12: held[max(0,a-pad):b+1+pad]=True; n+=1
    load.holds[p]=n; load.held[p]=held
    return x.astype(float).mean(1), r
load.holds={}; load.held={}
def env(x,r,hop=0.02):
    s=int(r*hop); n=len(x)//s; return np.sqrt((x[:n*s].reshape(n,s)**2).mean(1)), hop
def spec(x,r,t0,dur,held,nfft=2048):
    a=int(t0*r); seg=x[a:int((t0+dur)*r)]; hop=nfft//2; held=held[a:a+len(seg)]
    frames=[np.abs(np.fft.rfft(seg[i:i+nfft]*np.hanning(nfft))) for i in range(0,len(seg)-nfft,hop) if not held[i:i+nfft].any()]
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
sh,b=spec(h,hr,t_host,dur,load.held[sys.argv[1]]); sd,_=spec(d,dr,best[2]+t_host*best[1],dur,load.held[sys.argv[2]])
print(f"output holds (underruns, left out of the spectra): host {load.holds[sys.argv[1]]}, ds {load.holds[sys.argv[2]]}")
sh=20*np.log10(sh/sh.sum()+1e-12); sd=20*np.log10(sd/sd.sum()+1e-12)
diff=sd-sh
print('band  host-relative dB (ds - host), normalised levels:')
print(' '.join(f"{int(b[i]):>5}:{diff[i]:+.1f}" for i in range(24)))
hs=h[int(t_host*hr):int((t_host+dur)*hr)]; a=int((best[2]+t_host*best[1])*dr); ds=d[a:a+int(dur*dr)]
print(f"level ratio ds/host over the shared music {np.sqrt((ds**2).mean())/np.sqrt((hs**2).mean()):.2f}")
