"""Exercise production device display-latency lifecycle with a controlled clock/flush."""
from pathlib import Path
import tempfile,subprocess
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as d:
 p=Path(d)
 def put(name,s):
  f=p/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(s)
 put('core/usage_events.h','#pragma once\n#include <stdint.h>\nnamespace core{namespace usage{inline void emit(const char*,const char*,int64_t=-1){}}}')
 put('Arduino.h','#pragma once\n#include <stdint.h>\nextern uint32_t now; inline uint32_t millis(){return now;}\n')
 put('esp_random.h','#pragma once\ninline unsigned esp_random(){return 0x12345678;}\n')
 put('core/telemetry.h','#pragma once\nvoid record(const char*,...);\n#define LOGI(tag,...) record(__VA_ARGS__)\n')
 put('hal/hal_display.h','#pragma once\n#include <stdint.h>\nnamespace hal {namespace display {extern void (*observer)(uint32_t,int,int);inline void setFlushObserver(void(*p)(uint32_t,int,int)){observer=p;}}}\n')
 put('test.cpp',r'''
#include "services/agent_latency.h"
#include "hal/hal_display.h"
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>
#include <cassert>
uint32_t now=0;std::vector<std::string> logs;
namespace hal{namespace display{void(*observer)(uint32_t,int,int)=nullptr;}}
void record(const char* f,...){char b[200];va_list a;va_start(a,f);vsnprintf(b,sizeof(b),f,a);va_end(a);assert(std::string(b).size()<96);logs.emplace_back(b);}
using namespace services::agent_latency;
int main(){
 replyReady();painted(80,230);assert(!hal::display::observer&&logs.empty());
 now=100;begin();now=500;submit();submit();now=1500;replyReady();replyReady();painted(80,230);
 assert(logs.size()==4);assert(logs[1].find("e=submit ms=0")!=std::string::npos);
 auto cb=hal::display::observer;cb(1700,0,40);assert(logs.size()==4);cb(1800,80,232);
 assert(logs.back().find("e=display ms=1300 up=1800")!=std::string::npos);assert(!hal::display::observer);
 cancel();fail();assert(logs.size()==5);
 now=2000;begin();submit();now=2600;milestone("mic_stopped");assert(logs.back().find("ms=600 up=2600")!=std::string::npos);replyReady();painted(80,230);cancel();assert(!hal::display::observer);size_t n=logs.size();cb(2300,0,240);assert(logs.size()==n);
 now=0xfffffff0;begin();submit();now=20;replyReady();painted(80,230);hal::display::observer(40,0,240);assert(logs.back().find("ms=56 up=40")!=std::string::npos);
 begin();submit();fail();assert(logs.back().find("e=error")!=std::string::npos);
 puts("PASS first display once, partial/skipped flush, preview exclusion, cancel/error, duplicate milestones, clock wrap and 95-character cap");
}
''')
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(p),'-I'+str(ROOT/'include'),str(ROOT/'src/services/agent_latency.cpp'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
