"""Compile the actual HAL to test retained stack handoff and idle quarantine.

The controlled scheduler models cleanup ordering, including an idle iteration
preempted AFTER its cleanup. The device suite validates actual IDF scheduling.
"""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    t=Path(directory)
    def write(path,text):
        p=t/path;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    write('freertos/FreeRTOS.h', '''#pragma once
#include <stdint.h>
using BaseType_t=int;using UBaseType_t=unsigned;using StackType_t=uint8_t;
using TaskFunction_t=void(*)(void*);using TaskHandle_t=void*;struct StaticTask_t{int value;};
#define pdPASS 1
#define pdFAIL 0
''')
    write('freertos/task.h', '''#pragma once
#include "freertos/FreeRTOS.h"
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t,const char*,uint32_t,void*,UBaseType_t,TaskHandle_t*,BaseType_t);
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t,const char*,uint32_t,void*,UBaseType_t,StackType_t*,StaticTask_t*,BaseType_t);
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t);void vTaskDelete(TaskHandle_t);
''')
    write('Arduino.h', '''#pragma once
struct Logger{template<class...A>void printf(const char*,A...) {}};extern Logger Serial;
''')
    write('esp_heap_caps.h', '''#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1
inline size_t heap_caps_get_free_size(int){return 38000;}
inline size_t heap_caps_get_largest_free_block(int){return 3060;}
''')
    write('esp_freertos_hooks.h', '''#pragma once
#define ESP_OK 0
int esp_register_freertos_idle_hook_for_cpu(bool(*)(),unsigned);
''')
    write('test.cpp', r'''
#include "hal/hal_tasks.h"
#include "Arduino.h"
#include <cassert>
#include <cstdio>
Logger Serial;
bool (*hook)()=nullptr;bool (*audioHook)()=nullptr;bool registerFails=true,createFails=false,pendingCleanup=false;
unsigned staticCalls=0,dynamicCalls=0;StackType_t* firstStack=nullptr;
StaticTask_t* firstTcb=nullptr;StackType_t* audioStack=nullptr;StaticTask_t* audioTcb=nullptr;
int esp_register_freertos_idle_hook_for_cpu(bool(*fn)(),unsigned core){
 assert(core==0);if(registerFails)return -1;if(!hook)hook=fn;else audioHook=fn;return 0;
}
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t,const char*,uint32_t bytes,void*,UBaseType_t priority,StackType_t* stack,StaticTask_t* tcb,BaseType_t core){
 ++staticCalls;assert(core==0&&!pendingCleanup);
 if(bytes==3584){
  assert(priority==5);
  if(!audioStack){audioStack=stack;audioTcb=tcb;}
  assert(stack==audioStack&&tcb==audioTcb&&stack!=firstStack&&tcb!=firstTcb);
  return createFails?nullptr:tcb;
 }
 assert(((bytes==10240||bytes==12288)&&priority==1)||(bytes==4096&&priority==2));
 if(!firstStack){firstStack=stack;firstTcb=tcb;}
 assert(firstStack==stack&&firstTcb==tcb);return createFails?nullptr:tcb;
}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t,const char*,uint32_t,void*,UBaseType_t,TaskHandle_t*,BaseType_t){++dynamicCalls;return pdFAIL;}
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t){return 3000;}
void vTaskDelete(TaskHandle_t task){assert(!task&&!pendingCleanup);pendingCleanup=true;}
void entry(void*){}
int main(){using namespace hal::tasks;
 assert(!spawn(Id::UploadQ,entry)); // Hook failure rolls back ownership.
 registerFails=false;createFails=true;assert(!spawn(Id::NetPoll,entry));
 createFails=false;assert(spawn(Id::UploadQ,entry));
 assert(!spawn(Id::NetPoll,entry)&&!spawn(Id::UploadQ,entry));
 // The old idle iteration already ran cleanup, then the worker preempted it.
 deleteCurrent(Id::UploadQ);assert(pendingCleanup);
 hook();assert(!spawn(Id::NetPoll,entry)); // Must not reuse before NEXT cleanup.
 pendingCleanup=false;hook();assert(spawn(Id::NetPoll,entry));
 assert(dynamicCalls==0); // Both start even when the largest heap block is 10228.
 for(unsigned i=0;i<10000;++i){
  deleteCurrent(i%2?Id::UploadQ:Id::NetPoll);
  assert(!spawn(Id::UploadQ,entry));
  pendingCleanup=false;hook();assert(!spawn(Id::UploadQ,entry));hook();
  assert(spawn(i%2?Id::NetPoll:Id::UploadQ,entry));
 }
 deleteCurrent(Id::NetPoll);pendingCleanup=false;hook();hook();
 assert(spawn(Id::OtaDownload,entry));
 assert(!spawn(Id::UploadQ,entry)&&!spawn(Id::NetPoll,entry));
 deleteCurrent(Id::OtaDownload);hook();assert(!spawn(Id::NetPoll,entry));
 pendingCleanup=false;hook();assert(spawn(Id::UploadQ,entry));
 assert(dynamicCalls==0); // OTA reuses retained stack despite 3060-byte largest block.
 // Camera acquisition can reuse the excluded network workers' stack even
 // with only a 3060-byte heap block, but never before idle retirement cleanup.
 assert(!spawn(Id::VideoCapture,entry));
 deleteCurrent(Id::UploadQ);hook();assert(!spawn(Id::VideoCapture,entry));
 pendingCleanup=false;hook();assert(spawn(Id::VideoCapture,entry));
 assert(!spawn(Id::NetPoll,entry)&&!spawn(Id::UploadQ,entry)&&!spawn(Id::OtaDownload,entry));
 deleteCurrent(Id::VideoCapture);hook();assert(!spawn(Id::NetPoll,entry));
 pendingCleanup=false;hook();assert(spawn(Id::UploadQ,entry));
 assert(dynamicCalls==0);
 // Playback has independent admission: it can coexist with a network worker,
 // but MP3 and WAV must never share a live or not-yet-reclaimed stack.
 assert(spawn(Id::Mp3Play,entry));assert(audioHook);
 assert(!spawn(Id::WavPlay,entry));
 for(unsigned i=0;i<1000;++i){
  deleteCurrent(i%2?Id::WavPlay:Id::Mp3Play);
  assert(!spawn(Id::Mp3Play,entry)&&!spawn(Id::WavPlay,entry));
  audioHook();assert(!spawn(Id::WavPlay,entry));
  pendingCleanup=false;audioHook();
  assert(spawn(i%2?Id::Mp3Play:Id::WavPlay,entry));
 }
 assert(dynamicCalls==0);
 assert(!spawn(Id::QrScan,entry)&&dynamicCalls==1); // Other tasks stay dynamic.
 puts("PASS fragmented heap, duplicate admission, startup rollback, preempted idle cleanup and 10000 network and 1000 playback shared-stack handoffs");
}
''')
    subprocess.run(['c++','-std=c++14','-fsanitize=address,undefined','-g','-I'+str(t),'-I'+str(ROOT/'include'),str(t/'test.cpp'),str(ROOT/'src/hal/hal_tasks.cpp'),'-o',str(t/'test')],check=True)
    subprocess.run([str(t/'test')],check=True)
