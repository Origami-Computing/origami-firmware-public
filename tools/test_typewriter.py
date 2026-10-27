"""Compile the real typewriter against a recording HAL; pass a C++ compiler path."""
import pathlib, subprocess, sys
root=pathlib.Path(__file__).resolve().parents[1]
out=root/'.cache/typewriter-test';out.mkdir(parents=True,exist_ok=True)
files={
'Arduino.h':'#pragma once\n#include <stdint.h>\nuint32_t millis();\n',
'freertos/FreeRTOS.h':'#pragma once\n#define pdMS_TO_TICKS(x) (x)\n',
'freertos/task.h':'#pragma once\n',
'hal/hal_audio.h':'''#pragma once
#include <stddef.h>
#include <stdint.h>
#define ESP_OK 0
namespace hal { namespace audio {
bool configureForPlayback(); bool txBegin(uint32_t); bool txReady(); bool micReady();
void restoreForUiPlayback(); void txEnd(); uint32_t txGeneration();
int txWrite(const int16_t*,size_t,size_t*,unsigned); bool txFinishBuffer(unsigned);
}}
''',
'test.cpp':r'''
#include "sfx/sfx_typewriter.h"
#include <cassert>
#include <cstdint>
#include <vector>
#include <cstdio>
static uint32_t now=0,generation=0,rate=0;
static bool tx=false,mic=false,rejectWrite=false;
static unsigned starts=0,ends=0,flushes=0;
static std::vector<std::vector<int16_t>> blips;
uint32_t millis(){return now;}
namespace sfx {int scaledVolumeBasisPoints(){return 10000;}}
namespace hal {namespace audio {
bool configureForPlayback(){return true;}
bool txBegin(uint32_t r){rate=r;tx=true;++generation;++starts;return true;}
bool txReady(){return tx;} bool micReady(){return mic;}
void restoreForUiPlayback(){} void txEnd(){tx=false;++ends;}
uint32_t txGeneration(){return generation;}
int txWrite(const int16_t* p,size_t n,size_t* written,unsigned){
 if(rejectWrite){*written=0;return -1;}
 blips.emplace_back(p,p+n);*written=n*sizeof(int16_t);return 0;
}
bool txFinishBuffer(unsigned){++flushes;return true;}
}}
int main(){
 using namespace sfx;
 typewriterServiceChar(' ',0);assert(starts==0);
 typewriterServiceChar('H',0);assert(rate==16000&&blips.size()==1&&flushes==1);
 auto greeting=blips.back();assert(greeting.size()==256);
 for(size_t i=0;i<greeting.size();i+=2)assert(greeting[i]==greeting[i+1]);
 for(now=1;now<56;++now)typewriterServiceChar('a',now);
 assert(blips.size()==1);
 typewriterServiceChar('i',1);assert(blips.size()==2&&flushes==2);
 typewriterTeardown();assert(!typewriterIsOpen());
 typewriterServiceChar('H',0);assert(greeting==blips.back());
 // Foreign playback invalidates the cached session; the next character
 // must restore the 16 kHz greeting configuration.
 hal::audio::txBegin(22050);assert(!typewriterIsOpen());
 typewriterServiceChar('H',0);assert(rate==16000&&greeting==blips.back());
 hal::audio::txBegin(22050);auto previousEnds=ends;
 typewriterTeardown();assert(tx&&ends==previousEnds);
 mic=true;auto previousStarts=starts;
 typewriterServiceChar('a',1);assert(starts==previousStarts);
 mic=false;typewriterServiceChar('H',0);mic=true;
 typewriterServiceChar('a',1);assert(!tx&&!typewriterIsOpen());
 // The tutorial advances pitch only after successful playback, independent of
 // whitespace, reveal bursts and interval rejection. Preserve original samples.
 mic=false;
 for(const char* text:{"Hi, I am Kami", "personal sidekick, like Bmo", "rotate the scroll wheel"}){
  typewriterTeardown();unsigned note=0;
  for(const char* c=text;*c;++c){
   now+=17;
   if(typewriterServiceChar(*c,note))++note;
   now+=61;
   if(typewriterServiceChar(*c,note))++note;
  }
  const auto stats=typewriterStats();assert(stats.played==note&&note>5);
  for(unsigned n=0;n<5;++n)assert(stats.noteCounts[n]==note/5+(n<note%5?1:0));
 }
 now+=100;auto previous=typewriterStats().played;rejectWrite=true;
 assert(!typewriterServiceChar('X',0));assert(typewriterStats().played==previous);
 rejectWrite=false;assert(typewriterServiceChar('X',0));assert(blips.back()==greeting);
 assert(flushes==blips.size());
 puts("PASS unchanged greeting waveform, 56ms burst limit, DMA flush, TX ownership, mic guard");
}
'''}
for name,body in files.items():
    path=out/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(body)
compiler=[sys.argv[1]]+(['c++'] if pathlib.Path(sys.argv[1]).stem=='zig' else [])
binary=out/'test.exe'
subprocess.run(compiler+['-std=c++11','-I'+str(out),'-I'+str(root/'include'),str(out/'test.cpp'),str(root/'src/sfx/sfx_typewriter.cpp'),'-o',str(binary)],check=True)
subprocess.run([str(binary)],check=True)
