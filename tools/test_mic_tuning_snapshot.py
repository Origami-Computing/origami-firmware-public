"""Test the production mic DSP's frozen worker settings against its live UI mode."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'Arduino.h').write_text('#pragma once\n')
 (p/'Preferences.h').write_text('''#pragma once
#include <cstdint>
extern int preferencesAccesses;
struct Preferences {
bool begin(const char*,bool){++preferencesAccesses;return true;}void end(){}
uint16_t getUShort(const char*,uint16_t value){return value;}int8_t getChar(const char*,int8_t value){return value;}
int putUShort(const char*,uint16_t){return 2;}int putChar(const char*,int8_t){return 1;}
};
''')
 (p/'test.cpp').write_text(r'''
#include "core/mic_tuning.h"
#include <array>
#include <cassert>
#include <cstring>
#include <random>
#include <thread>
int preferencesAccesses=0;
using namespace core::audio::mic_tuning;
int main(){
 Settings snapshot;snapshot.gainBasisPoints=23456;snapshot.highPassHz=100;snapshot.limiterDbfs=-3;
 Processor frozen,live;frozen.resetFromSnapshot(snapshot);assert(preferencesAccesses==0);
 setGainBasisPoints(snapshot.gainBasisPoints);setHighPassHz(snapshot.highPassHz);setLimiterDbfs(snapshot.limiterDbfs);live.reset();
 std::mt19937 rng(42);int32_t raw[256];int16_t a[256],b[256];
 for(auto& sample:raw)sample=int32_t(int16_t(rng()))*65536;
 for(int i=0;i<100;++i){live.processRaw(raw,a,256);frozen.processRaw(raw,b,256);assert(!memcmp(a,b,sizeof(a)));}
 assert(live.samples()==frozen.samples()&&live.sumSquares()==frozen.sumSquares()&&live.peak()==frozen.peak());
 setGainBasisPoints(0);live.processRaw(raw,a,256);for(auto sample:a)assert(sample==0);
 frozen.processRaw(raw,b,256);bool audible=false;for(auto sample:b)audible|=sample!=0;assert(audible);
 // Snapshot validation matches the established setters at every range edge.
 Settings invalid;invalid.gainBasisPoints=65535;invalid.highPassHz=65535;invalid.limiterDbfs=100;
 frozen.resetFromSnapshot(invalid);setGainBasisPoints(65535);setHighPassHz(65535);setLimiterDbfs(100);live.reset();
 live.processRaw(raw,a,256);frozen.processRaw(raw,b,256);assert(!memcmp(a,b,sizeof(a)));
 // A worker keeps its owned snapshot while the UI changes the global controls.
 Processor reference;reference.resetFromSnapshot(snapshot);frozen.resetFromSnapshot(snapshot);
 uint64_t expected=0,actual=0;
 for(int i=0;i<2000;++i){reference.processRaw(raw,a,256);for(auto sample:a)expected=expected*131+uint16_t(sample);}
 std::thread worker([&]{for(int i=0;i<2000;++i){frozen.processRaw(raw,b,256);for(auto sample:b)actual=actual*131+uint16_t(sample);}});
 for(int i=0;i<2000;++i){setGainBasisPoints(i%40001);setHighPassHz(i%501);setLimiterDbfs(-(i%21));}
 worker.join();assert(actual==expected&&frozen.samples()==512000);
 // Switching back to the existing live mode follows the current UI controls.
 setGainBasisPoints(0);frozen.reset();frozen.processRaw(raw,b,256);for(auto sample:b)assert(sample==0);
}
''')
 subprocess.run(['c++','-std=c++17','-pthread','-fsanitize=address,undefined','-I'+str(p),'-I'+str(ROOT/'include'),str(ROOT/'src/core/mic_tuning.cpp'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS production mic DSP: live/snapshot parity, no worker persistence access, validation, concurrent UI edits and reset')
