"""Compare the firmware product-sound renderer with a port of the Swift reference.

The reference (ProductSound.samples, desk-sounds.ts) is transcribed below in
Python with double precision. Firmware stores recipe fields as float, so the
comparison allows a small tolerance; noise voices use the same seeded LCG.
"""
from pathlib import Path
import math, struct, subprocess, tempfile

ROOT = Path(__file__).resolve().parents[1]
RATE = 22050
SEED = 12345
NAMES = ['WaterDrop','AddMenu','Typing','TalkStart','Send','Cancel','Lock','LibraryOpen','LibraryClose']

def V(**k):
    v = dict(frequency=0, end=None, delay=0.0, attack=0.006, decay=0.12, gain=0.1, cutoff=1000, quality=0.5, noise=False)
    v.update(k); return v

def recipe(name, j):
    if name == 'WaterDrop': return [V(frequency=700*j, end=1100*j, attack=.003, decay=.04, gain=.15, cutoff=None)]
    if name == 'AddMenu': return [V(frequency=420*j, end=760*j, attack=.003, decay=.07, gain=.13, cutoff=None)]
    if name == 'Typing': return [V(frequency=900*j, attack=.001, decay=.006, gain=.05, quality=.8, noise=True), V(frequency=220*j, attack=.003, decay=.035, gain=.05, cutoff=700)]
    if name == 'TalkStart': return [V(frequency=392*j, decay=.08, gain=.07), V(frequency=523*j, delay=.06, gain=.06)]
    if name in ('Send','Cancel'): return [V(frequency=300*j, end=480*j, attack=.003, decay=.03, gain=.05, cutoff=1200), V(frequency=700*j, end=900*j, delay=.015, attack=.01, decay=.2, gain=.016, quality=.6, noise=True)]
    if name == 'Lock': return [V(frequency=150*j, end=300*j, attack=.004, decay=.06, gain=.14, cutoff=900), V(frequency=75*j, attack=.004, decay=.05, gain=.06, cutoff=400)]
    if name == 'LibraryOpen': return [V(frequency=140*j, end=112*j, delay=.06, decay=.13, gain=.15, cutoff=600), V(frequency=70*j, delay=.06, gain=.07, cutoff=300)]
    if name == 'LibraryClose': return [V(frequency=125*j, end=100*j, decay=.1, gain=.11, cutoff=550), V(frequency=62*j, decay=.09, gain=.05, cutoff=300)]

M64 = (1 << 64) - 1
def reference(voices, rate, seed):
    duration = max(v['delay'] + v['attack'] + v['decay'] + .05 for v in voices)
    out = [0.0] * math.ceil(duration * rate)
    rnd = seed
    for v in voices:
        phase = x1 = x2 = y1 = y2 = 0.0
        start = int(v['delay'] * rate); length = int((v['attack'] + v['decay'] + .05) * rate)
        end = v['end'] if v['end'] is not None else v['frequency']
        for i in range(length):
            if start + i >= len(out): break
            t = i / rate
            ramp = v['decay'] if v['cutoff'] is None else v['attack'] + v['decay']
            f = v['frequency'] * (end / v['frequency']) ** min(1, t / ramp)
            rnd = (rnd * 6364136223846793005 + 1) & M64
            s = (rnd >> 11) / (M64 >> 11) * 2 - 1 if v['noise'] else math.sin(phase)
            phase += 2 * math.pi * f / rate
            if v['cutoff'] is not None:
                w = 2 * math.pi * (f if v['noise'] else v['cutoff']) / rate
                c = math.cos(w); a = math.sin(w) / (2 * 10 ** (v['quality'] / 20))
                b0 = (1 - c) / 2; b1 = 1 - c; a0 = 1 + a
                y = (b0 * s + b1 * x1 + b0 * x2 + 2 * c * y1 - (1 - a) * y2) / a0
                x2 = x1; x1 = s; y2 = y1; y1 = y; s = y
            env = v['gain'] * t / v['attack'] if t < v['attack'] else v['gain'] * (0.0001 / v['gain']) ** min(1, (t - v['attack']) / v['decay'])
            out[start + i] += s * env * 0.6
    return out

code = r'''
#include "sfx/product_sounds.h"
#include <cstdio>
#include <vector>
using namespace sfx::product;
int main(){
  for(int s=0;s<9;++s){
    Recipe r=recipe((Sound)s,1.0f);
    size_t n=frames(r,%d.f);
    std::vector<float> out(n);
    render(r,%d.f,%dULL,out.data(),n);
    fwrite(&n,sizeof(n),1,stdout);fwrite(out.data(),sizeof(float),n,stdout);
    size_t shortN=frames(r,%d.f,false);
    if(shortN>n) return 2;
  }
}
''' % (RATE, RATE, SEED, RATE)

with tempfile.TemporaryDirectory() as d:
    p = Path(d); (p/'t.cpp').write_text(code)
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(p/'t.cpp'),'-o',str(p/'t')],check=True)
    data = subprocess.run([str(p/'t')],check=True,capture_output=True).stdout

at = 0
for name in NAMES:
    (n,) = struct.unpack_from('Q', data, at); at += 8
    got = struct.unpack_from('%df' % n, data, at); at += 4 * n
    ref = reference(recipe(name, 1.0), RATE, SEED)
    assert len(ref) == n, (name, len(ref), n)
    peak = max(abs(x) for x in ref)
    err = max(abs(a - b) for a, b in zip(got, ref))
    assert peak > 0.005, (name, peak)
    assert err <= peak * 0.02 + 1e-5, (name, err, peak)
print('PASS product sounds: all nine recipes match the reference renderer within 2% of peak; short form never longer')
