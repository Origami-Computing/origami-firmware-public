"""Compile real microphone capture/DSP against deterministic raw HAL reads.

Checks streamed PCM, clipping, partial blocks, burst limits and hardware entry
policy. Does not prove microphone timing, acoustic quality or concurrent use.
"""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='kami-mic-') as directory:
    root=Path(directory)
    def write(name,text):
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    write('Arduino.h', '''#pragma once
#include <stdint.h>
#include <stddef.h>
#define pdMS_TO_TICKS(x) (x)
struct Logger {void println(const char*){}template<class...T>void printf(const char*,T...) {}};
extern Logger Serial;void delay(uint32_t);
''')
    write('Wire.h','')
    write('Pins.h','')
    write('esp_err.h','#pragma once\n#define ESP_OK 0\n')
    write('hal/hal_audio.h','''#pragma once
#include <stddef.h>
#include <stdint.h>
namespace hal {namespace audio {
void hardwareReset();bool initCodec(uint8_t);void configureForRecording();
bool codecReady();bool initMic(bool streaming=false);int readMic(void*,size_t,size_t*,uint32_t);void endMicSession();
}}
''')
    write('test.cpp',r'''
#include "services/mic_capture.h"
#include <Arduino.h>
#include <cassert>
#include <cstring>
#include <vector>
#include <deque>
Logger Serial;
int resets=0,configured=0,starts=0,stops=0,warmReads=0,dataReads=0;
bool micOk=true,readFails=false,codecValid=false;int codecAddress=0x1a;
std::vector<int> addresses;std::deque<std::vector<int32_t>> blocks;
void delay(uint32_t ms){assert(ms==40);}
namespace hal {namespace audio {
void hardwareReset(){++resets;codecValid=false;}
bool codecReady(){return codecValid;}
bool initCodec(uint8_t a){addresses.push_back(a);codecValid=a==codecAddress;return codecValid;}
void configureForRecording(){++configured;}
bool initMic(bool streaming){assert(streaming);++starts;return micOk;}
void endMicSession(){++stops;}
int readMic(void* out,size_t cap,size_t* bytes,uint32_t timeout){
 *bytes=0;
 if(timeout){assert(timeout==2 && cap==512);++warmReads;return 0;}
 ++dataReads;assert(cap==2048);
 if(readFails)return -1;
 if(blocks.empty())return 0;
 auto b=blocks.front();blocks.pop_front();assert(b.size()*4<=cap);
 *bytes=b.size()*4;memcpy(out,b.data(),*bytes);return 0;
}
}}
std::vector<int16_t> received;int callbacks=0;
bool sink(const int16_t* values,uint32_t n,void* user){
 assert(user==&callbacks);++callbacks;received.insert(received.end(),values,values+n);
 return false; // Existing contract ignores backpressure; do not change silently.
}
int32_t raw(int16_t v){return int32_t(v)*65536;}
int main(){
 using services::audio::MicCapture;
 MicCapture mic;
 assert(!mic.startStream(nullptr,nullptr));
 assert(resets==1&&configured==1&&starts==0&&mic.isPrepared());
 assert((addresses==std::vector<int>{0x18,0x19,0x1a}));
 micOk=false;assert(!mic.startStream(sink,&callbacks));assert(starts==1&&stops==0);
 micOk=true;assert(mic.startStream(sink,&callbacks));
 assert(resets==1&&starts==2&&warmReads==4); // Empty warmup stops after four reads.
 blocks={{raw(3),raw(6),raw(9),raw(-3),raw(-6),raw(-9),
          raw(32767),raw(32767),raw(32767),raw(-32768),raw(-32768),raw(-32768),raw(99)},
         {raw(12),raw(12),raw(12)}};
 mic.tick();assert((received==std::vector<int16_t>{7,-7,32767,-32768,15}));
 assert(mic.samplesN()==5&&callbacks==2); // Incomplete raw group is discarded per read.
 for(int i=0;i<17;++i)blocks.push_back({raw(8),raw(8),raw(8)});
 int before=dataReads;mic.tick();assert(dataReads-before==16&&blocks.size()==1);
 assert(mic.samplesN()==21&&callbacks==18); // False sink result does not end capture.
 mic.tick();assert(mic.samplesN()==22&&blocks.empty());
 readFails=true;before=dataReads;mic.tick();assert(dataReads==before+1&&mic.samplesN()==22);
 readFails=false;mic.stop();assert(stops==1&&mic.samplesN()==22);
 before=dataReads;mic.tick();assert(dataReads==before); // No reads after stop.
 assert(mic.startStream(sink,&callbacks)&&mic.samplesN()==0&&resets==1);
 mic.stop();assert(stops==2);
 codecAddress=-1;codecValid=false;MicCapture absent;
 assert(absent.startStream(sink,&callbacks)); // Existing I2S-only fallback.
 assert(resets==2&&configured==1&&addresses.size()==7);
 absent.stop();assert(stops==3);
}
''')
    binary=root/'test'
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(root),'-I'+str(ROOT/'include'),str(ROOT/'src/services/mic_capture.cpp'),str(root/'test.cpp'),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS real capture: codec fallback, startup/failure order, PCM averaging/gain/clipping, partial blocks, 16-burst cap, sink contract, stop/restart')
