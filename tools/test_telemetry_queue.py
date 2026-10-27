"""Production telemetry queue: burst retention, reserved slots, loss and snapshot safety."""
from pathlib import Path
import subprocess, tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    p = Path(directory)
    def put(name, text):
        f = p / name
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_text(text)
    put('core/usage_events.h','#pragma once\nnamespace core{namespace usage{inline void init(){}}}')
    put('Arduino.h', r'''#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <cstdlib>
inline size_t strlcpy(char* d,const char* s,size_t n){size_t k=strlen(s);if(n){size_t c=k<n-1?k:n-1;memcpy(d,s,c);d[c]=0;}return k;}
struct String { std::string s; size_t length(){return s.size();} int indexOf(char){return -1;} String substring(int,int=0){return {};} int toInt(){return 0;} const char* c_str(){return s.c_str();} };
struct SerialT {template<class... A> size_t printf(A...){return 0;} void println(const char*){}}; static SerialT Serial;
''')
    put('SD.h', r'''#pragma once
#include "Arduino.h"
#define FILE_WRITE 1
#define FILE_APPEND 2
#define FILE_READ 3
struct File {operator bool(){return false;} template<class... A> size_t printf(A...){return 0;} void flush(){} void close(){} bool available(){return false;} String readStringUntil(char){return {};} };
struct SDT {bool remove(const char*){return true;} bool rename(const char*,const char*){return true;} bool exists(const char*){return false;} void mkdir(const char*){} File open(const char*,int){return {};}}; static SDT SD;
''')
    put('esp_heap_caps.h', '#pragma once\n#include <cstdlib>\n#define MALLOC_CAP_SPIRAM 1\ninline void* heap_caps_malloc(size_t n,int){return malloc(n);}\n')
    put('freertos/FreeRTOS.h', '#pragma once\n#define pdMS_TO_TICKS(n) (n)\n#define pdTRUE 1\n')
    put('freertos/semphr.h', '#pragma once\nusing SemaphoreHandle_t=int*; extern bool deny; inline int* xSemaphoreCreateMutex(){static int x;return &x;} inline int xSemaphoreTake(int*,int){return !deny;} inline void xSemaphoreGive(int*){}\n')
    put('hal/hal_bus.h','#pragma once\nnamespace hal {namespace bus {inline bool lockSpi(int){return false;} inline void unlockSpi(){}}}\n')
    put('hal/hal_sd.h','#pragma once\nnamespace hal {namespace sd {inline bool mounted(){return false;}}}\n')
    put('test.cpp',r'''
#include "core/telemetry_queue.h"
#include <cassert>
#include <cstring>
#include <cstdio>
bool deny=false;
using namespace core::telemetry_queue;
int main(){
 init();
 for(int i=0;i<kMaxItems-8;i++){char b[30];snprintf(b,sizeof(b),"event-%d",i);enqueue(KIND_VERBOSE,b);}
 enqueue(KIND_VERBOSE,"dropped"); assert(stats().droppedVerbose==1);
 Item snapshotItems[16];assert(snapshot(snapshotItems,16)==16);
 for(int i=0;i<8;i++)enqueue(KIND_LOG,"durable");
 enqueue(KIND_LOG,"full");assert(stats().droppedDurable==1);
 Item head;assert(snapshot(&head,1)==1);assert(!strcmp(head.json,"event-0"));
 ack(16);assert(stats().queued==kMaxItems-16);
 assert(snapshot(&head,1)==1);assert(!strcmp(head.json,"event-16"));
 deny=true;enqueue(KIND_VERBOSE,"locked");enqueue(KIND_CRASH,"locked");deny=false;
 assert(stats().droppedVerbose==2&&stats().droppedDurable==2);
 ack(kMaxItems);assert(!hasPending());enqueue(KIND_VERBOSE,"recovered");
 assert(snapshot(&head,1)==1);assert(!strcmp(head.json,"recovered"));
 puts("PASS burst retention, durable reserve, loss counters, in-flight prefix and recovery");
}
''')
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(p),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'.pio/libdeps/esp32-s3-menu-n16r8/ArduinoJson/src'),str(ROOT/'src/core/telemetry_queue.cpp'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)
