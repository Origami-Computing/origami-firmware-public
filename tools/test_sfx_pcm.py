"""Exercise production SFX PCM output/session handling against a faultable HAL."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    (temp / 'hal').mkdir()
    (temp / 'Arduino.h').write_text('''#pragma once
#include <cstdint>
#include <cstddef>
using esp_err_t = int;
constexpr int ESP_OK = 0;
#define pdMS_TO_TICKS(n) (n)
uint32_t millis();
''')
    (temp / 'hal/hal_audio.h').write_text('''#pragma once
#include <Arduino.h>
namespace hal { namespace audio {
bool codecReady(); void hardwareReset(); bool initCodec(uint8_t);
bool configureForPlayback(); uint32_t codecConfigGeneration();
bool micReady(); bool txReady(); uint32_t txGeneration();
bool txBegin(uint32_t); void txEnd();
int txWrite(const int16_t*, size_t, size_t*, unsigned);
bool txFinishBuffer(unsigned);
}}
''')
    (temp / 'test.cpp').write_text(r'''
#include <cassert>
#include <vector>
#include "sfx/sfx_voice.h"
#include "sfx/camera_shutter.h"
uint32_t now=100,gen=0,codecGen=1;
uint32_t millis(){return now;}
int gain=10000,writes=0,finishes=0,begins=0,ends=0,failAt=0;
bool mic=false,tx=false,codec=true,initOk=true,configOk=true,beginOk=true;
bool partial=false,finishOk=true;
std::vector<int16_t> output;
namespace sfx { int scaledVolumeBasisPoints(){return gain;} }
namespace hal { namespace audio {
bool codecReady(){return codec;} void hardwareReset(){}
bool initCodec(uint8_t){return codec=initOk;}
bool configureForPlayback(){++codecGen;return configOk;}
uint32_t codecConfigGeneration(){return codecGen;}
bool micReady(){return mic;} bool txReady(){return tx;}
uint32_t txGeneration(){return gen;}
bool txBegin(uint32_t rate){assert(rate==22050);++begins;if(!beginOk)return false;tx=true;++gen;return true;}
void txEnd(){++ends;tx=false;}
int txWrite(const int16_t* p,size_t n,size_t* bytes,unsigned timeout){
 assert(n<=1024&&timeout==40);++writes;
 *bytes=n*2-(partial?2:0);
 if(failAt==writes)return -1;
 output.insert(output.end(),p,p+n);return 0;
}
bool txFinishBuffer(unsigned timeout){assert(timeout==40);++finishes;return finishOk;}
}}
void reset(){sfx::resetPlaybackSessionState();tx=mic=false;codec=true;
 gain=10000;writes=finishes=begins=ends=failAt=0;partial=false;
 initOk=configOk=beginOk=finishOk=true;output.clear();}
int main(){
 const auto clip=sfx::kCameraShutter;
 assert(clip.frames==7497); // 340ms, no lead/tail silence in the firmware.
 // Keep the recorded cue in the UI-cue level range without digital clipping.
 // This protects against accidentally regenerating the original quiet asset.
 double energy=0;int peak=0;
 for(size_t i=0;i<clip.frames;++i){int s=clip.samples[i];energy+=double(s)*s;if(s<0)s=-s;if(s>peak)peak=s;}
 assert(peak>=28000&&peak<=29200);
 assert(energy/clip.frames>0.12*0.12*32768.*32768.);
 assert(energy/clip.frames<0.20*0.20*32768.*32768.);
 reset();assert(sfx::play(clip));assert(output.size()==clip.frames*2);
 for(size_t i=0;i<clip.frames;++i)assert(output[i*2]==clip.samples[i]&&output[i*2+1]==clip.samples[i]);
 assert(writes==15&&finishes==1&&begins==1);
 assert(sfx::play(clip)&&begins==1); // same owned session reused
 sfx::endPlaybackSessionIfIdle(now+7999);assert(!ends);
 sfx::endPlaybackSessionIfIdle(now+8000);assert(ends==1);
 for(int g:{0,2000,5000,10000}){
  reset();gain=g;assert(sfx::play(clip));
  if(!g){assert(!begins&&output.empty());continue;}
  for(size_t i=0;i<clip.frames;++i)assert(output[2*i]==int32_t(clip.samples[i])*g/10000);
 }
 const int16_t extremes[]={-32768,-1,0,1,32767};
 reset();assert(sfx::play(sfx::PcmClip{extremes,5}));assert(output.front()==-32768&&output.back()==32767);
 reset();assert(!sfx::play(sfx::PcmClip{nullptr,5}));assert(!sfx::play(sfx::PcmClip{extremes,0}));assert(!begins);
 reset();mic=true;assert(!sfx::play(clip)&&!begins&&!writes);
 reset();tx=true;assert(!sfx::play(clip)&&!begins&&!writes); // foreign playback
 reset();codec=false;initOk=false;assert(!sfx::play(clip)&&!writes);
 reset();configOk=false;assert(!sfx::play(clip)&&!writes);
 reset();beginOk=false;assert(!sfx::play(clip)&&!writes);
 reset();failAt=2;assert(!sfx::play(clip)&&writes==2&&finishes==1);
 reset();partial=true;assert(!sfx::play(clip)&&writes==1&&finishes==1);
 reset();finishOk=false;assert(!sfx::play(clip));
 reset();assert(sfx::play(clip));++gen;sfx::endPlaybackSessionIfIdle(now+9000);assert(!ends); // owner changed
 reset();static unsigned progress=0;progress=0;
 assert(sfx::playAndRelease(clip, [](){assert(tx&&writes>=2);++progress;}));
 assert(progress==17&&ends==1&&!tx&&finishes==1);
 assert(output.size()==clip.frames*2+3*1024);
 for(size_t i=clip.frames*2;i<output.size();++i)assert(output[i]==0);
 reset();tx=true;assert(!sfx::playAndRelease(clip)&&!ends&&!writes); // foreign TX preserved
 reset();mic=true;assert(!sfx::playAndRelease(clip)&&!ends&&!writes);
 reset();assert(sfx::play(clip));gain=0;assert(sfx::playAndRelease(clip)&&ends==1&&!tx);
 reset();failAt=16;assert(!sfx::playAndRelease(clip)&&ends==1&&!tx); // drain fails, still release
 reset();partial=true;assert(!sfx::playAndRelease(clip)&&ends==1&&!tx);
 reset();assert(sfx::beginPlaybackSession());sfx::SfxVoice voice;
 voice.start({SFX_SQUARE,1000,1000,0.01f,0.5f,0,0,0,0});
 while(voice.pump()){}assert(output.size()==440&&finishes==1);
}
''')
    subprocess.run([
        'c++', '-std=c++17', '-fsanitize=address,undefined', '-g',
        '-I' + str(temp), '-I' + str(ROOT / 'include'),
        str(ROOT / 'src/sfx/sfx_voice.cpp'), str(ROOT / 'src/sfx/camera_shutter.cpp'),
        str(temp / 'test.cpp'), '-o', str(temp / 'test'),
    ], check=True)
    subprocess.run([str(temp / 'test')], check=True)

print('PASS real PCM/synth rendering, gains, session ownership/reuse, mute and output failures')
