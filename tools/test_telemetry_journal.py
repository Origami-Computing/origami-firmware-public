import os
"""Production SD journal replay, short writes and interrupted rename recovery."""
from pathlib import Path
import tempfile,subprocess
root=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as d:
 p=Path(d)
 def put(name,text):
  q=p/name;q.parent.mkdir(parents=True,exist_ok=True);q.write_text(text)
 put('Arduino.h',r'''#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <cstdlib>
inline size_t strlcpy(char*d,const char*s,size_t n){size_t k=strlen(s);if(n){auto c=k<n-1?k:n-1;memcpy(d,s,c);d[c]=0;}return k;}
struct String{std::string s;size_t length(){return s.size();}int indexOf(char c){auto p=s.find(c);return p==s.npos?-1:int(p);}String substring(int a,int b=-1){return {s.substr(a,b<0?std::string::npos:b-a)};}int toInt(){return atoi(s.c_str());}const char*c_str(){return s.c_str();}};
struct SerialT{template<class...A>void printf(A...){}void println(const char*){}};inline SerialT Serial;
''')
 put('SD.h',r'''#pragma once
#include "Arduino.h"
#include <map>
#include <cstdio>
#define FILE_WRITE 1
#define FILE_APPEND 2
#define FILE_READ 3
inline std::map<std::string,std::string> disk;
inline bool shortWrite=false,failPromotion=false;
inline int rewrites=0;
struct File{
 std::string name;size_t pos=0;operator bool(){return !name.empty();}
 template<class...A>size_t printf(const char* fmt,A...a){char b[1024];int n=snprintf(b,sizeof b,fmt,a...);if(shortWrite)n/=2;disk[name].append(b,n);return n;}
 void flush(){}void close(){}bool available(){return pos<disk[name].size();}
 String readStringUntil(char c){auto& s=disk[name];auto e=s.find(c,pos);if(e==s.npos)e=s.size();auto out=s.substr(pos,e-pos);pos=e+1;return {out};}
};
struct SDT{
 bool remove(const char*p){disk.erase(p);return true;}
 bool rename(const char*a,const char*b){if(failPromotion&&std::string(a).find(".tmp")!=std::string::npos)return false;if(!disk.count(a))return false;disk[b]=disk[a];disk.erase(a);return true;}
 bool exists(const char*p){return disk.count(p);}void mkdir(const char*){}
 File open(const char*p,int mode){if(mode==FILE_READ&&!disk.count(p))return {};if(mode==FILE_WRITE){disk[p]="";++rewrites;}return {p,0};}
};inline SDT SD;
''')
 put('core/usage_events.h','#pragma once\nnamespace core{namespace usage{inline void init(){}}}')
 put('esp_heap_caps.h','#pragma once\n#include <cstdlib>\n#define MALLOC_CAP_SPIRAM 1\ninline void* heap_caps_malloc(size_t n,int){return malloc(n);}')
 put('freertos/FreeRTOS.h','#pragma once\n#define pdMS_TO_TICKS(n) (n)\n#define pdTRUE 1')
 put('freertos/semphr.h','#pragma once\nusing SemaphoreHandle_t=int*;inline int*xSemaphoreCreateMutex(){static int x;return &x;}inline int xSemaphoreTake(int*,int){return 1;}inline void xSemaphoreGive(int*){}')
 put('hal/hal_bus.h','#pragma once\nnamespace hal{namespace bus{inline bool lockSpi(int){return true;}inline void unlockSpi(){}}}')
 put('hal/hal_sd.h','#pragma once\nnamespace hal{namespace sd{inline bool mounted(){return true;}}}')
 put('main.cpp',r'''
#include "core/telemetry_queue.h"
#include "SD.h"
#include <cassert>
using namespace core::telemetry_queue;
int main(){
 init();enqueue(KIND_LOG,"{\"seq\":1}");enqueue(KIND_VERBOSE,"volatile");
 init();Item rows[8];assert(snapshot(rows,8)==1);assert(strstr(rows[0].json,"1"));
 enqueue(KIND_USAGE,"{\"seq\":2}");
 shortWrite=true;ack(1);shortWrite=false;
 assert(stats().journalFailures>0);
 init();assert(snapshot(rows,8)==2); // retained original, including safe duplicate
 failPromotion=true;ack(2); // crash between old->backup and tmp->journal
 assert(disk.count("/telemetry_queue/journal.bak"));
 failPromotion=false;init();assert(snapshot(rows,8)==2);
 ack(2);init();assert(snapshot(rows,8)==0);
 enqueue(KIND_USAGE,"{\"seq\":3}");init();assert(snapshot(rows,8)==1);
 ack(1);init();
 for(int i=0;i<100;++i)enqueue(KIND_USAGE,"{\"seq\":1}");
 int before=rewrites;
 for(int i=0;i<25;++i)ack(4);
 assert(rewrites-before==4); // 32,64,96 and empty, rather than 25 full rewrites.
 init();assert(snapshot(rows,8)==0);
 enqueue(KIND_USAGE,"{\"event\":\"retained\"}");enqueue(KIND_USAGE,"{\"event\":\"pending\"}");ack(1);
 init();assert(snapshot(rows,8)==2); // safe replay of acknowledged usage + pending tail
 ack(2);init();
 for(int i=0;i<512;++i)enqueue(KIND_USAGE,"{\"event\":\"bounded\"}");
 ack(4); // journal still has 512 durable records
 failPromotion=true;auto drops=stats().droppedDurable;enqueue(KIND_USAGE,"{\"event\":\"must-not-overflow\"}");
 assert(stats().droppedDurable==drops+1);failPromotion=false;
 init();int count=0;Item all[512];count=snapshot(all,512);assert(count==512);
 disk["/telemetry_queue/journal.jsonl"]="4\t{\"seq\":\n4\t{\"seq\":9}\n";
 init();assert(snapshot(rows,8)==1);assert(strstr(rows[0].json,"9")); // torn line cannot poison FIFO
 ack(1);init();shortWrite=true;enqueue(KIND_USAGE,"{\"seq\":10}");shortWrite=false;
 enqueue(KIND_USAGE,"{\"seq\":11}");init();assert(snapshot(rows,8)==2);
 puts("PASS durable replay, volatile exclusion, short-write preservation, interrupted promotion recovery, committed ack and re-entry");
}
''')
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(p),'-I'+str(root/'include'),'-I'+os.environ.get('ARDUINOJSON_SRC',str(root/'.pio/libdeps/esp32-s3-menu-n16r8/ArduinoJson/src')),str(root/'src/core/telemetry_queue.cpp'),str(p/'main.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
