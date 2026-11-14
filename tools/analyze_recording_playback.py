"""Relative acoustic PCM16 measurements; never calibrated SPL.

Requires numpy. Explicit intervals exclude navigation sounds and playback edges.
Reject noise subtraction if pre/post noise changes >3 dB or playback is <10 dB
above the louder noise interval. This cannot undo microphone AGC/noise reduction.
"""
import argparse
import hashlib
import json
from pathlib import Path
import wave
import numpy as np


def analyze(path, pre, playback, post):
    with wave.open(str(path)) as stream:
        if stream.getsampwidth()!=2:raise ValueError('Expected PCM16')
        rate=stream.getframerate()
        channels=stream.getnchannels()
        samples=np.frombuffer(stream.readframes(stream.getnframes()),dtype='<i2').astype(float)
        samples=samples.reshape(-1,channels).mean(axis=1)/32768
    def db(power):return float(10*np.log10(max(power,1e-20)))
    def interval(bounds):
        if not 0<=bounds[0]<bounds[1]<=len(samples)/rate:raise ValueError('Interval outside capture')
        x=samples[int(bounds[0]*rate):int(bounds[1]*rate)]
        size=rate//10
        blocks=x[:len(x)//size*size].reshape(-1,size)
        power=np.mean(blocks**2,axis=1)
        return dict(seconds=bounds,rms_dbfs=db(np.mean(x*x)),
                    window_p10_dbfs=db(np.percentile(power,10)),
                    window_p90_dbfs=db(np.percentile(power,90)),
                    below_minus90_fraction=float(np.mean(power<1e-9)),
                    peak_dbfs=db(np.max(x*x)))
    before,active,after=map(interval,(pre,playback,post))
    noise=max(before['rms_dbfs'],after['rms_dbfs'])
    margin=active['rms_dbfs']-noise
    drift=abs(before['rms_dbfs']-after['rms_dbfs'])
    reasons=[]
    if margin<10:reasons.append('playback less than 10 dB above background')
    if drift>3:reasons.append('pre/post background differs by more than 3 dB')
    if max(before['below_minus90_fraction'],after['below_minus90_fraction'])>.25:
        reasons.append('near-zero microphone windows: check gating/AGC before linear subtraction')
    if max(before['window_p90_dbfs']-before['window_p10_dbfs'],
           after['window_p90_dbfs']-after['window_p10_dbfs'])>10:
        reasons.append('background varies by more than 10 dB within intervals')
    return dict(file=path.name,sha256=hashlib.sha256(path.read_bytes()).hexdigest(),
                duration_seconds=len(samples)/rate,pre=before,playback=active,post=after,
                playback_above_background_db=margin,noise_drift_db=drift,
                valid_for_noise_subtraction=not reasons,rejection_reasons=reasons,
                noise_corrected_dbfs=None if reasons else db(10**(active['rms_dbfs']/10)-10**(noise/10)),
                limitation='Relative microphone level; not SPL. Host audio processing and placement must remain fixed.')


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('wav',type=Path)
    for name in ('pre','playback','post'):p.add_argument('--'+name,nargs=2,type=float,required=True)
    p.add_argument('--output',type=Path)
    a=p.parse_args();result=analyze(a.wav,a.pre,a.playback,a.post)
    text=json.dumps(result,indent=2)
    if a.output:a.output.write_text(text+'\n')
    print(text)
