"""Exercise the real v7 progress service with faultable persistence."""
from pathlib import Path
import os, shlex, subprocess, tempfile
ROOT=Path(__file__).resolve().parents[1]
code=r'''
#include <cassert>
#include <cstring>
#include <map>
#include <string>
#include "services/onboarding_progress.h"
std::map<std::string,unsigned> values;bool readOk=true,writeOk=true;unsigned writes=0;
struct Preferences {
 bool begin(const char*,bool){return readOk;}
 unsigned getUInt(const char* key,unsigned fallback){auto i=values.find(key);return i==values.end()?fallback:i->second;}
 bool getBool(const char* key,bool fallback){return getUInt(key,fallback);}
 void end(){}
};
namespace core {namespace store {
 bool putU32(const char*,const char* key,unsigned value){++writes;if(!writeOk)return false;values[key]=value;return true;}
 bool putBool(const char* ns,const char* key,bool value){return putU32(ns,key,value);}
}}
'''
code+=(ROOT/'src/services/onboarding_progress.cpp').read_text()
code+=r'''
int main(){
 using namespace services::onboarding_progress;
 for(unsigned interest=0;interest<3;++interest)for(unsigned flags=0;flags<8;++flags)for(unsigned page=0;page<=141;++page){
  if(page==44)continue;
  const bool done=flags&1,account=flags&2,wifi=flags&4;
  assert(save(page,done,interest,account,wifi));auto p=load();
  unsigned expected=page;
  if(page==12)expected=account&&interest?40:41;
  if(page==19||page==20)expected=18;
  if(page==23)expected=22;if(page==27)expected=25;
  if(page==30||page==31||page==43)expected=40;
  if(page>=129&&page<=134)expected=128;if(page==124)expected=123;
  assert(p.page==expected&&p.completed==done&&p.interest==interest&&p.account==account&&p.joinWifi==wifi);
  unsigned before=writes;assert(save(page,done,interest,account,wifi));assert(writes==before);
 }
 values.clear();s_saved=0;assert(load().page==0&&!load().completed);
 values["guide_v5"]=0x4b050119;assert(load().completed);
 values["guide_v5"]=0x4b050019;assert(!load().completed&&load().page==0);
 values["guide_v6"]=0xffffffff;assert(!load().completed);
 values.clear();s_saved=0;writeOk=false;
 assert(!save(37,false,2,true,true));assert(s_saved==0);
 writeOk=true;assert(save(37,false,2,true,true));assert(load().interest==2);
 readOk=false;unsigned saved=s_saved;assert(load().page==0&&s_saved==saved);readOk=true;
 assert(resetForDevelopment());assert(!load().completed&&load().page==0);
 assert(save(12,false,0,true,true,true,true));auto resume=load();assert(resume.page==40&&resume.setupPublished&&resume.connected);
 assert(!save(44,false)&&!save(0,false,3));
}
'''
with tempfile.TemporaryDirectory(dir=ROOT/'.cache') as d:
 p=Path(d);(p/'test.cpp').write_text(code);(p/'Preferences.h').write_text('#pragma once\n')
 compiler=shlex.split(os.environ.get('CXX','c++'))
 subprocess.run(compiler+['-std=c++14','-DORIGAMI_DEV_SERIAL_INPUT','-I'+str(p),'-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True,timeout=10)
print('PASS v7 route persistence, all checkpoints, migration, deduplication, failed writes and reset')
