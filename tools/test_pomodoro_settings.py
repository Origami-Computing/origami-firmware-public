"""Exercise actual Pomodoro persistence: byte normalization and partial saves.

Storage is fake; current core vibration validation and settings service compile
under ASan/UBSan. This checks schema/ordering/failures, not NVS hardware.
"""
from pathlib import Path
import subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
source=r'''#include "services/pomodoro_settings.h"
#include "core/store.h"
#include "core/alert_vibration.h"
#include <cassert>
#include <cstring>
#include <cstdio>
uint8_t values[4]; unsigned reads=0,writes=0,failures=0; bool missing=false;
const char* keys[]={"work","break","sound","vibration"};
const uint8_t defaults[]={25,5,0,0};
namespace core {namespace store {
uint8_t getU8(const char* ns,const char* key,uint8_t def){
 assert(!strcmp(ns,"pomodoro")&&reads<4&&!strcmp(key,keys[reads])&&def==defaults[reads]);
 return missing?defaults[reads++]:values[reads++];
}
bool putU8(const char* ns,const char* key,uint8_t value){
 assert(!strcmp(ns,"pomodoro")&&writes<4&&!strcmp(key,keys[writes]));
 unsigned index=writes++;if(failures&(1u<<index))return false;
 values[index]=value;return true;
}
}}
int main(){
 services::pomodoro::Settings s;
 assert(s.focusMinutes==25&&s.breakMinutes==5&&!s.sound&&!s.vibration);
 missing=true;s.focusMinutes=3;s.load();assert(reads==4&&s.focusMinutes==25&&s.breakMinutes==5&&!s.sound&&!s.vibration);
 missing=false;
 // Every byte value independently passes through each persisted field.
 for(unsigned value=0;value<256;++value){
  reads=0;for(auto& v:values)v=value;s.load();
  unsigned minutes=value<1?1:value>99?99:value;
  assert(reads==4&&s.focusMinutes==minutes&&s.breakMinutes==minutes);
  assert(s.sound==value%4&&s.vibration==(value<core::alert_vibration::kCount?value:0));
 }
 reads=0;values[0]=42;values[1]=7;values[2]=2;values[3]=1;s.load();
 assert(s.focusMinutes==42&&s.breakMinutes==7&&s.sound==2&&s.vibration==1);
 for(failures=0;failures<16;++failures){
  writes=0;for(auto& v:values)v=0;
  // Save preserves caller bytes: normalization belongs to load, not writes.
  s.focusMinutes=255;s.breakMinutes=0;s.sound=254;s.vibration=253;
  const uint8_t expected[]={255,0,254,253};
  assert(s.save()==(failures==0)&&writes==4);
  for(unsigned i=0;i<4;++i)assert(values[i]==((failures&(1u<<i))?0:expected[i]));
  assert(s.focusMinutes==255&&s.breakMinutes==0&&s.sound==254&&s.vibration==253);
 }
 puts("PASS defaults, 256 bytes per field, read/write order, all 16 failure combinations and unchanged draft");
}
'''
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory)
 # Compile the real validator, excluding unrelated pattern/storage operations.
 production=(ROOT/'src/core/alert_vibration.cpp').read_text()
 a=production.index('uint8_t validId(');b=production.index('}',a)+1
 validator='#include "core/alert_vibration.h"\nnamespace core {namespace alert_vibration {'+production[a:b]+'}}\n'
 (p/'test.cpp').write_text(source);(p/'validator.cpp').write_text(validator)
 binary=p/'test'
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(p/'test.cpp'),str(p/'validator.cpp'),str(ROOT/'src/services/pomodoro_settings.cpp'),'-o',str(binary)],check=True)
 subprocess.run([str(binary)],check=True)
