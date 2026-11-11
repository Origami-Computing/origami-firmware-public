"""Exercise the real snapshot ownership and fresh-frame loop with faultable frames."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    def write(name,text):
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    write('services/camera_session.h', '''#pragma once
#include <stdint.h>
#include <stddef.h>
#define HAL_CAMERA_PIXFORMAT_JPEG 4
namespace hal {namespace camera {struct CameraFrame {const uint8_t* buf;size_t len;unsigned width,height;int format;uint64_t dmaStartUs;};}}
namespace services {namespace camera {class Session {public:bool read(hal::camera::CameraFrame*);void returnFrame();};}}
''')
    write('esp_heap_caps.h','''#include <stddef.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
void* heap_caps_malloc(size_t,int);void heap_caps_free(void*);
''')
    write('esp_timer.h','#include <stdint.h>\nint64_t esp_timer_get_time();')
    write('main.cpp',r'''
#include "services/camera_snapshot.h"
#include "services/camera_session.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <vector>
using namespace services::camera;
uint8_t jpeg[]={255,216,42,255,217};
std::vector<hal::camera::CameraFrame> frames;
unsigned reads=0,returns=0,allocs=0,frees=0;bool held=false,allocFail=false;
int64_t now=100,step=0;
int64_t esp_timer_get_time(){auto value=now;now+=step;return value;}
void* heap_caps_malloc(size_t n,int caps){assert(caps==3);assert(held);if(allocFail)return nullptr;++allocs;return malloc(n);}
void heap_caps_free(void*p){if(p){++frees;free(p);}}
bool Session::read(hal::camera::CameraFrame*f){assert(!held);++reads;if(frames.empty())return false;*f=frames.front();frames.erase(frames.begin());held=true;return true;}
void Session::returnFrame(){assert(held);held=false;++returns;}
hal::camera::CameraFrame frame(uint64_t timestamp=100){return {jpeg,sizeof(jpeg),640,480,4,timestamp};}
void reset(){assert(!held);frames.clear();reads=returns=0;now=100;step=0;allocFail=false;}
int main(){Session session;
{Snapshot shot;
 frames={frame(99),frame(100)};assert(shot.captureVga(session,100)==Snapshot::Result::Ready);
 assert(reads==2&&returns==2&&shot.timing().stale==1&&shot.timing().dmaUs==100);
 assert(shot.size()==sizeof(jpeg)&&shot.data()!=jpeg&&!memcmp(shot.data(),jpeg,sizeof(jpeg)));
 shot.clear();shot.clear();assert(!shot.data()&&!shot.size()&&allocs==frees);
 reset();frames.assign(9,frame(99));assert(shot.captureVga(session,100)==Snapshot::Result::FrameUnavailable);assert(reads==8&&returns==8);
 reset();step=1500000;frames={frame(99),frame()};assert(shot.captureVga(session,100)==Snapshot::Result::FrameUnavailable);assert(reads==1&&returns==1);
 reset();now=1500100;frames={frame()};assert(shot.captureVga(session,100)==Snapshot::Result::FrameUnavailable&&reads==0);
 reset();assert(shot.captureVga(session,100)==Snapshot::Result::FrameUnavailable&&returns==0);
 for(int kind=0;kind<4;++kind){reset();auto bad=frame();if(kind==0)bad.format=0;if(kind==1)bad.width=320;if(kind==2)bad.len=3;if(kind==3)bad.buf=nullptr;frames={bad,frame()};assert(shot.captureVga(session,100)==Snapshot::Result::FrameUnavailable);assert(reads==1&&returns==1);}
 reset();allocFail=true;frames={frame()};assert(shot.captureVga(session,100)==Snapshot::Result::OutOfMemory);assert(returns==1&&!shot.data()&&!shot.size());
 reset();frames={frame(),frame()};assert(shot.captureVga(session,100)==Snapshot::Result::Ready);assert(shot.captureVga(session,100)==Snapshot::Result::Ready);assert(allocs==frees+1);
 reset();assert(shot.captureVga(session,100)==Snapshot::Result::FrameUnavailable);assert(!shot.data()&&allocs==frees);
 reset();frames={frame()};assert(shot.captureVga(session,100)==Snapshot::Result::Ready);
}assert(allocs==frees&&!held);
}
''')
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(root),'-I'+str(ROOT/'include'),str(ROOT/'src/services/camera_snapshot.cpp'),str(root/'main.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
    subprocess.run(['c++','-std=c++11','-I'+str(ROOT/'include'),'-c',str(ROOT/'test/test_camera_capture_policy/test_compile.cpp'),'-o',str(root/'policy.o')],check=True)
print('PASS snapshot freshness, retry/time bounds, invalid frames, allocation failure, copies and repeated/destructor cleanup under sanitizers')
