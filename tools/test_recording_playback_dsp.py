"""Compile production recording DSP; test bounds, chunking, rates and dynamics.

Optional --input/--output PCM16 files run the exact production processor on a
private recording for acoustic A/B tests (no recordings are committed).
"""
import argparse
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = r'''
#include "core/recording_playback_dsp.h"
#include "core/audio_dsp.h"
#include <cassert>
#include <vector>
#include <fstream>
#include <random>
#include <algorithm>
using core::audio::RecordingPlaybackDsp;
int main(int argc, char** argv) {
 for (unsigned rate : {8000u,16000u,44100u,48000u}) {
  RecordingPlaybackDsp dsp; dsp.reset(rate);
  int16_t silence[512]={};dsp.process(silence,512,1);
  for(auto s:silence)assert(s==0);
  std::vector<int16_t> quiet(rate);
  for(unsigned i=0;i<rate;++i)quiet[i]=int16_t(200*std::sin(i*6.283185307*440/rate));
  auto reference=quiet;dsp.process(quiet.data(),quiet.size(),1);
  double before=0,after=0;
  for(unsigned i=0;i<rate;++i){before+=double(reference[i])*reference[i];after+=double(quiet[i])*quiet[i];}
  const double expectedGainSquared=double(RecordingPlaybackDsp::kMaxGain)*RecordingPlaybackDsp::kMaxGain;
  assert(after/before>expectedGainSquared*.998 && after/before<expectedGainSquared*1.001);
  std::mt19937 rng(42);std::vector<int16_t> a(rate*2);
  for(auto& s:a)s=int16_t(rng());a[0]=-32768;a[1]=32767;
  auto b=a;dsp.reset(rate);dsp.process(a.data(),rate,2);
  RecordingPlaybackDsp split;split.reset(rate);
  for(unsigned i=0;i<rate;){unsigned n=std::min(137u,rate-i);split.process(b.data()+2*i,n,2);i+=n;}
  assert(a==b);for(auto s:a){
   assert(s>=-27852 && s<=27852);
   const auto speaker=core::dsp::applyVolumeBasisPoints(s,2000);
   assert(speaker>=-5570 && speaker<=5570);
   assert(core::dsp::applyVolumeBasisPoints(s,0)==0);
  }
  // Stereo linking preserves equal channels and polarity.
  int16_t stereo[]={32767,-32767,500,-500};dsp.reset(rate);dsp.process(stereo,2,2);
  assert(stereo[0]==-stereo[1] && stereo[2]==-stereo[3]);
  // A transient cannot pin gain for the rest of the file; recovery is bounded.
  dsp.reset(rate);int16_t spike=-32768;dsp.process(&spike,1,1);assert(spike==-27852);
  std::vector<int16_t> tail(rate,100);dsp.process(tail.data(),tail.size(),1);assert(tail.back()==100*RecordingPlaybackDsp::kMaxGain);
  dsp.reset(rate);auto replay=reference;dsp.process(replay.data(),replay.size(),1);assert(replay==quiet);
 }
 if(argc==3){
  std::ifstream input(argv[1],std::ios::binary);std::ofstream output(argv[2],std::ios::binary);
  assert(input&&output);RecordingPlaybackDsp dsp;dsp.reset(48000);int16_t block[1024];
  while(input.read(reinterpret_cast<char*>(block),sizeof(block)) || input.gcount()){
   auto bytes=input.gcount();assert(bytes%2==0);dsp.process(block,bytes/2,1);
   output.write(reinterpret_cast<char*>(block),bytes);
  }
 }
}
'''

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input',type=Path)
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    if bool(args.input)!=bool(args.output):parser.error('input and output required together')
    with tempfile.TemporaryDirectory() as directory:
        p=Path(directory);(p/'test.cpp').write_text(SOURCE)
        subprocess.run(['c++','-std=c++17','-O2','-fsanitize=address,undefined',
                        '-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
        subprocess.run([str(p/'test')]+([str(args.input),str(args.output)] if args.input else []),check=True)
    print('PASS recording playback DSP: bounds, silence, gain, transient recovery, stereo linking, reset and chunk invariance')
