"""Run the production preview worker with threaded codec/HAL boundaries."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as d:
 p=Path(d)
 def put(name,s):
  target=p/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_text(s)
 put('core/preview_decoder.h',r'''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <atomic>
#include <thread>
#include <chrono>
extern std::atomic<bool> decoding,hold,failDecode;
extern bool failPrepare,honorAbort,lastCrop,lastFit;
namespace core {namespace images {class PreviewDecoder {public:
 static constexpr size_t kPixels=280*240;enum class Result {Decoded,Invalid,Interrupted};
 bool prepareNative(bool crop){lastCrop=crop;return !failPrepare;} void close(){if(decoding)std::abort();}
 Result decode(const uint8_t* p,size_t,uint16_t* out,bool(*abort)(void*),void* user,bool fit){lastFit=fit;decoding=true;while(hold){if(honorAbort&&abort&&abort(user)){decoding=false;return Result::Interrupted;}std::this_thread::sleep_for(std::chrono::milliseconds(1));}for(size_t i=0;i<kPixels;++i)out[i]=p[6];decoding=false;return failDecode?Result::Invalid:Result::Decoded;}
};}}
''')
 put('hal/hal_tasks.h',r'''#pragma once
#include <thread>
#include <chrono>
#define pdMS_TO_TICKS(x) (x)
inline void vTaskDelay(unsigned n){std::this_thread::sleep_for(std::chrono::milliseconds(n));}
namespace hal {namespace tasks {enum class Id {VideoPreview};void* spawn(Id,void(*)(void*),void*);void recordCurrentStack(Id);void deleteCurrent(Id);}}
''')
 put('esp_heap_caps.h',r'''#pragma once
#include <cstdlib>
#include <atomic>
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
extern unsigned allocAttempt,failAllocation;extern std::atomic<int> allocations;
inline void* heap_caps_malloc(size_t n,int){if(++allocAttempt==failAllocation)return nullptr;auto*p=malloc(n);if(p)++allocations;return p;}
inline void* heap_caps_aligned_calloc(size_t,size_t n,size_t size,int caps){return heap_caps_malloc(n*size,caps);}
inline void heap_caps_free(void*p){if(p){--allocations;free(p);}}
''')
 put('esp_timer.h','#pragma once\n#include <chrono>\ninline uint64_t esp_timer_get_time(){return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}\n')
 put('main.cpp',r'''
#include "services/camera_preview_decode.h"
#include "hal/hal_tasks.h"
#include <cassert>
#include <vector>
std::atomic<bool> decoding{false},hold{false},failDecode{false};bool failPrepare=false,failSpawn=false,honorAbort=false,lastCrop=false,lastFit=false;
unsigned allocAttempt=0,failAllocation=0;std::atomic<int> allocations{0};std::thread worker;
namespace hal {namespace tasks {
void* spawn(Id,void(*fn)(void*),void* p){if(failSpawn)return nullptr;assert(!worker.joinable());worker=std::thread(fn,p);return p;}
void recordCurrentStack(Id){}void deleteCurrent(Id){}
}}
void join(){if(worker.joinable())worker.join();}
template<class F>void until(F fn){for(int i=0;i<2000;++i){if(fn())return;vTaskDelay(1);}assert(false);}
int main(){using services::camera::PreviewDecode;PreviewDecode p;
 for(unsigned n=1;n<=4;++n){allocAttempt=0;failAllocation=n;assert(!p.begin()&&!allocations);}
 failAllocation=0;failPrepare=true;assert(!p.begin()&&!allocations);failPrepare=false;
 failSpawn=true;assert(!p.begin()&&!allocations);failSpawn=false;
 std::vector<uint8_t> jpeg={255,216,255,254,0,3,17,255,192,0,2,255,218,0,2,42,255,217};
 assert(p.begin());assert(!p.submit(nullptr,12,1));hold=true;
 assert(p.submit(jpeg.data(),jpeg.size(),10));until([]{return decoding.load();});
 jpeg[6]=34;assert(p.submit(jpeg.data(),jpeg.size(),20));assert(!p.submit(jpeg.data(),jpeg.size(),30));
 std::fill(jpeg.begin(),jpeg.end(),0);hold=false;
 PreviewDecode::Frame frame{};until([&]{return p.acquire(frame);});assert(frame.sensorUs==10&&frame.pixels[0]==17&&lastCrop&&!lastFit);
 vTaskDelay(20);assert(frame.pixels[0]==17&&frame.pixels[67199]==17);assert(!p.acquire(frame));p.release();
 until([&]{return p.acquire(frame);});assert(frame.sensorUs==20&&frame.pixels[0]==34);p.release();p.close();join();assert(!allocations&&!p.active());
 jpeg={255,216,255,254,0,3,51,255,192,0,2,255,218,0,2,42,255,217};
 assert(p.begin());hold=true;assert(p.submit(jpeg.data(),jpeg.size(),40));until([]{return decoding.load();});
 std::thread unblock([]{vTaskDelay(30);assert(allocations==4);hold=false;});p.close();unblock.join();join();assert(!allocations&&!decoding);
 assert(p.begin());failDecode=true;assert(p.submit(jpeg.data(),jpeg.size(),50));until([&]{return p.failed();});assert(!p.acquire(frame));p.close();join();assert(!allocations);failDecode=false;
 assert(p.begin(true)&&!p.failed()&&!lastCrop);assert(p.submit(jpeg.data(),jpeg.size(),60));
 until([&]{return p.acquire(frame);});assert(lastFit&&frame.sensorUs==60&&frame.pixels[0]==51);p.release();p.close();join();
 assert(p.begin(true));honorAbort=true;hold=true;assert(p.submit(jpeg.data(),jpeg.size(),70));until([]{return decoding.load();});
 p.close();join();assert(!allocations&&!decoding&&!p.failed());hold=false;honorAbort=false;
 assert(p.begin());p.close();join();assert(!allocations);
}
''')
 subprocess.run(['c++','-std=c++17','-pthread','-fsanitize=address,undefined','-I'+str(p),'-I'+str(ROOT/'include'),str(p/'main.cpp'),str(ROOT/'src/services/camera_preview_decode.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
print('PASS production preview worker: bounded copies/borrows, backpressure, failure rollback, joined cancellation and re-entry')
