"""Compare the original diagnostic with the extracted service under injected faults.

Compiles actual old/new app entry functions and current service. Deterministic
HAL/storage stubs check frame lifetime, full operation traces, logs and CSV rows.
"""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
BASELINES = ROOT/'test/fixtures/operation_baselines'
def entry(source, name):
    start = source.index('bool runSettleTrial(')
    text = source[start:source.index('\n#endif', start)]
    return text.replace('runSettleTrial(', name+'(').replace(
        'NavHandler::instance().top() != &app || camState != CAM_VIEWFINDER', 'badView')
old = (BASELINES/'camera_trial.cpp').read_text()
new = (ROOT/'src/apps/camera_menu_app.cpp').read_text()
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    def write(name,text):
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    write('Arduino.h',r'''#pragma once
#include <stdint.h>
#include <stddef.h>
#include <cstdio>
#include <cstring>
#include <string>
extern std::string trace, logs;
struct Logger {
 void println(const char* s){logs+=std::string(s)+"\n";}
 template<class... A> void printf(const char* f,A... a){char b[1024];snprintf(b,sizeof(b),f,a...);logs+=b;}
};extern Logger Serial;
void delay(unsigned);uint32_t millis();
''')
    write('services/camera_session.h',r'''#pragma once
#include <Arduino.h>
namespace hal {namespace camera {struct Tuning {int ae_level=0,wb_mode=0,gainceiling_idx=1,saturation=0,contrast=0,sharpness=0;};}}
namespace services {namespace camera {class Session {
public: enum class PrepareResult {Ready,BackgroundBusy,AgentBusy};
 bool running() const;void stop();PrepareResult prepareCapture(bool);bool start(int,int,const hal::camera::Tuning*);
};}}
''')
    write('esp_camera.h',r'''#pragma once
#include <Arduino.h>
struct camera_fb_t {uint8_t* buf;size_t len;uint16_t width,height;int format;struct {long tv_sec,tv_usec;} timestamp;};
constexpr int FRAMESIZE_VGA=8,PIXFORMAT_JPEG=4;
camera_fb_t* esp_camera_fb_get();void esp_camera_fb_return(camera_fb_t*);
''')
    write('SD.h',r'''#pragma once
#include <Arduino.h>
constexpr int FILE_APPEND=1;
struct File {bool ok;explicit operator bool()const{return ok;}size_t println(const char*);void close();};
struct Card {bool exists(const char*);File open(const char*,int);};extern Card SD;
''')
    write('hal/hal_bus.h','namespace hal {namespace bus {void lockSpi();void unlockSpi();}}')
    write('hal/hal_sd.h',r'''#pragma once
#include <Arduino.h>
namespace hal {namespace sd {bool mounted();bool ensureDir(const char*);bool exists(const char*);bool writeFile(const char*,const uint8_t*,size_t);bool removeFile(const char*);}}
''')
    write('main.cpp',r'''
#include "services/camera_trial.h"
#include <Arduino.h>
#include <esp_camera.h>
#include <SD.h>
#include "hal/hal_sd.h"
#include "hal/hal_bus.h"
#include <cassert>
#include <iostream>
#include <vector>
std::string trace,logs,manifest;Logger Serial;Card SD;
int fault=0,frameRequests=0;uint32_t clockMs=100;bool held=false,badView=false,photo=false;
uint8_t jpeg[]={255,216,3,4,255,217};camera_fb_t frame={jpeg,6,640,480,4,{12,345}};
void delay(unsigned n){trace+="delay"+std::to_string(n)+";";clockMs+=n;}
uint32_t millis(){return clockMs;}
namespace services {namespace camera {
bool Session::running()const{return fault!=1;}
void Session::stop(){assert(!held);trace+="stop;";}
Session::PrepareResult Session::prepareCapture(bool deep){assert(!deep);trace+="prepare;";return fault==5?PrepareResult::BackgroundBusy:fault==6?PrepareResult::AgentBusy:PrepareResult::Ready;}
bool Session::start(int size,int format,const hal::camera::Tuning*){assert(size==8&&format==4);trace+="start;";clockMs+=700;return fault!=7;}
}}
camera_fb_t* esp_camera_fb_get(){assert(!held);trace+="get;";++frameRequests;clockMs+=fault==15?9000:90;if((fault==8&&frameRequests==1)||(fault==9&&frameRequests==3))return nullptr;held=true;frame.len=fault==10?2:6;return &frame;}
void esp_camera_fb_return(camera_fb_t* f){assert(f==&frame&&held);trace+="return;";held=false;}
namespace hal {namespace sd {
bool mounted(){return fault!=2;}
bool ensureDir(const char* p){trace+=std::string("dir:")+p+";";return fault!=3;}
bool exists(const char*){return photo||fault==4;}
bool writeFile(const char*,const uint8_t* p,size_t n){assert(held&&p==jpeg&&n==6);trace+="write;";clockMs+=40;photo=true;return fault!=11;}
bool removeFile(const char*){assert(!held);trace+="remove;";photo=false;return true;}
}namespace bus {void lockSpi(){assert(!held);trace+="lock;";}void unlockSpi(){trace+="unlock;";}}}
bool Card::exists(const char*){return fault==14;}
File Card::open(const char*,int mode){assert(mode==FILE_APPEND);trace+="manifest;";return {fault!=12};}
size_t File::println(const char* s){manifest+=std::string(s)+"\r\n";return strlen(s)+(fault==13?1:2);}
void File::close(){trace+="close;";}
services::camera::Session s_camera;
bool s_videoPending=false;
struct {bool active=false;bool busy(){return active;}}s_video;
hal::camera::Tuning effectiveTuning(){return {};}
const char* kModeLabels[]={"Standard"};int s_cameraMode=0;
struct Settings {int mode=0;hal::camera::Tuning tuning(){return effectiveTuning();}}s_settings;
bool camReportPreparation(services::camera::Session::PrepareResult r){trace+="report"+std::to_string((int)r)+";";return r==services::camera::Session::PrepareResult::Ready;}
bool camStopWifiForCamera(bool deep){return camReportPreparation(s_camera.prepareCapture(deep));}
void camShowShutterFrame(){trace+="shutter;";clockMs+=200;}
bool camShowCapturedPreview(const char*,const char*){assert(!held);trace+="review;";clockMs+=30;return true;}
bool camRestorePreviewAfterCapture(uint32_t ms){assert(!held&&ms==500);trace+="restore;";clockMs+=800;return fault!=16;}
'''+entry(old,'runOld')+'\n'+entry(new,'runNew')+r'''
struct Result {bool ok;std::string operations,messages,csv;bool operator==(const Result&r)const{return ok==r.ok&&operations==r.operations&&messages==r.messages&&csv==r.csv;}};
Result run(bool newer,uint32_t id,int frames,unsigned repeat,int injected,bool invalidView){
 fault=injected;badView=invalidView;trace.clear();logs.clear();manifest.clear();clockMs=100;frameRequests=0;held=photo=false;
 bool ok=newer?runNew(id,frames,repeat):runOld(id,frames,repeat);assert(!held);
 return {ok,trace,logs,manifest};
}
int main(){int cases=0;
 for(int frames:{-2,-1,0,2,30,31})for(int f=0;f<=16;++f){
  auto a=run(false,77,frames,1,f,false),b=run(true,77,frames,1,f,false);
  if(!(a==b)){std::cerr<<"Mismatch frames="<<frames<<" fault="<<f<<"\n"<<a.operations<<"\n"<<b.operations<<"\n";return 1;}++cases;
 }
 for(unsigned repetition:{0u,1u,9u,10u})for(uint32_t id:{0u,77u})for(bool invalid:{false,true}){
  assert(run(false,id,2,repetition,0,invalid)==run(true,id,2,repetition,0,invalid));++cases;
 }
 for(int gate:{0,1}){
  s_video.active=gate==0;s_videoPending=gate==1;
  assert(!run(true,77,2,1,0,false).ok&&trace.empty());
 }
 s_video.active=false;s_videoPending=false;
 assert(run(true,77,2,1,0,false).ok);
 for(int f=1;f<=16;++f)if(f!=14)assert(!run(true,77,2,1,f,false).ok);
 std::cout<<"PASS "<<cases<<" old/new diagnostic cases: ordering, timing, logs, CSV, frame release and failures\n";
}
''')
    subprocess.run(['c++','-std=c++11','-DORIGAMI_DEV_SERIAL_INPUT','-fsanitize=address,undefined','-I'+str(root),'-I'+str(ROOT/'include'),str(ROOT/'src/services/camera_trial.cpp'),str(root/'main.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
