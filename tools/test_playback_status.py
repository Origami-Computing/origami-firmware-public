"""Check elapsed publication against the old arithmetic and real WAV spectrum.

Concurrent reader checks exercise atomic publication, not physical task timing.
Spectrum bars remain independently sampled, not a coherent frame snapshot.
"""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
def function(text,signature):
 a=text.index(signature);b=text.index('{',a)+1;depth=1
 while depth:depth+=(text[b]=='{')-(text[b]=='}');b+=1
 return text[a:b]
header=(ROOT/'include/services/wav_player.h').read_text()
source=(ROOT/'src/services/wav_player.cpp').read_text()
code=r'''
#include "core/playback_progress.h"
#include "core/pcm_ring.h"
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>
std::vector<int16_t> accepted;
size_t acceptLimit=0;
namespace hal { namespace audio {
int txWrite(const int16_t* p,size_t n,size_t* written,int) {
 size_t take=n<acceptLimit?n:acceptLimit;
 accepted.insert(accepted.end(),p,p+take);*written=take*sizeof(int16_t);return 0;
}
}}
class WavPlayer {public:
 static constexpr int kVisBars=16;
 std::atomic<uint8_t> spectrum_[kVisBars]{};
 void analyzeVis_(const int16_t*,int,int);
 core::dsp::PcmRing ring_{};
 core::audio::PlaybackProgress outputProgress_;
 uint32_t outputSampleRemainder_=0;
 struct {int hz=48000;} info_;
 void drainToI2S_();
'''
code+=function(header,'void copySpectrum(')+'\n'+function(header,'void clearSpectrum()')+'\n};\n'
code+=function(source,'void WavPlayer::analyzeVis_(')
code+=function(source,'void WavPlayer::drainToI2S_(')
code+=r'''
int main(){
 core::audio::PlaybackProgress progress;std::mt19937 rng(135);
 for(int rate:{0,8000,16000,22050,44100,48000,96000}){
  uint64_t frames=0;progress.reset();assert(progress.elapsedMs()==0);
  for(int i=0;i<20000;++i){
   if(i%17==0){uint32_t ms=rng();frames=rate>0?(uint64_t)ms*rate/1000:0;progress.seek(ms,rate);}
   else{uint32_t n=rng();frames+=n;progress.add(n,rate);}
   assert(progress.elapsedMs()==(rate>0?(uint32_t)(frames*1000ULL/(uint64_t)rate):0));
  }
 }
 progress.reset();std::atomic<bool> done{false};
 std::thread writer([&]{for(int i=0;i<100000;++i)progress.add(1,1000);done.store(true);});
 uint32_t previous=0;while(!done.load()){
  uint32_t value=progress.elapsedMs();assert(value>=previous && value<=100000);previous=value;
 }writer.join();assert(progress.elapsedMs()==100000);
 WavPlayer player;uint8_t bars[16];player.clearSpectrum();player.copySpectrum(bars);
 for(auto bar:bars)assert(bar==0);
 for(int channels:{1,2})for(int frames=0;frames<=320;++frames){
  std::vector<int16_t> samples((frames+1)*channels);
  for(auto& sample:samples)sample=int16_t(rng());
  player.analyzeVis_(samples.data(),frames,channels);player.copySpectrum(bars);
  int perBar=frames/16;if(perBar<1)perBar=1;
  for(int b=0;b<16;++b){
   int32_t peak=0;
   for(int i=b*perBar;i<frames && i<(b+1)*perBar;++i){
    int32_t value=channels==1?samples[i]:samples[2*i]/2+samples[2*i+1]/2;
    if(value<0)value=-value;if(value>peak)peak=value;
   }
   uint32_t expected=(uint32_t)peak*255u/32768u;if(expected>255)expected=255;
   assert(bars[b]==expected);
  }
  player.clearSpectrum();player.copySpectrum(bars);for(auto bar:bars)assert(bar==0);
 }
 int16_t storage[4096];player.ring_.buf=storage;player.ring_.cap=4096;player.ring_.reset();
 std::vector<int16_t> input(3001);for(size_t i=0;i<input.size();++i)input[i]=int16_t(i);
 player.ring_.push(input.data(),input.size());
 // Buffered-but-unsubmitted audio must not move the output clock. Repeated
 // zero and odd-sample short writes must neither duplicate nor lose PCM.
 for(size_t limit:{0u,1u,0u,7u,511u,0u,13u,512u}) {
   acceptLimit=limit;player.drainToI2S_();
   assert(player.outputProgress_.elapsedMs()==(accepted.size()/2)*1000/48000);
   assert(player.outputSampleRemainder_==accepted.size()%2);
   assert(player.ring_.used()==int(input.size()-accepted.size()));
 }
 acceptLimit=512;player.drainToI2S_();assert(accepted==input);
 puts("PASS elapsed arithmetic/publication, WAV spectrum, I2S output clock and partial-write PCM retention");
}
'''
with tempfile.TemporaryDirectory() as directory:
 root=Path(directory);(root/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++11','-pthread','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(root/'test.cpp'),str(ROOT/'src/core/pcm_ring.cpp'),'-o',str(root/'test')],check=True)
 subprocess.run([str(root/'test')],check=True)
