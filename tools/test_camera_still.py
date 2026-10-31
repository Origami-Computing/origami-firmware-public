"""Compile the still-preparation service and real session/reservation with faultable HALs."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    def write(name,text):
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    write('hal/hal_camera.h',r'''#pragma once
#include <stdint.h>
#include <stddef.h>
namespace hal {namespace camera {
struct Tuning {int tag=42;};struct CameraFrame {};
bool init(int,int,const Tuning*,int);void deinit();bool initialized();void settleAutoExposure(uint32_t);
bool getFrame(CameraFrame*);void returnFrame();bool captureToFile(const char*);bool captureToSd();const char* lastInitError();size_t availablePsramKb();
}}
''')
    write('Arduino.h',r'''#pragma once
#include <stdint.h>
struct Logger {void println(const char*);template<class... Args> void printf(const char*,Args...) {}};extern Logger Serial;
void delay(unsigned);uint32_t millis();
''')
    write('esp_camera.h','enum framesize_t {FRAMESIZE_VGA=8,FRAMESIZE_HD=11,FRAMESIZE_QSXGA=21};constexpr int PIXFORMAT_JPEG=4;')
    write('hal/hal_wifi.h','namespace hal {namespace wifi {void stop();void powerDown();void restore();void scheduleRestore();}}')
    write('hal/hal_io.h','namespace hal {namespace io {bool reinitAfterI2CRestart();}}')
    write('Pins.h','constexpr int SDA_PIN=1,SCL_PIN=2;')
    write('Wire.h','struct FakeWire {void end(){}void begin(int,int){}};extern FakeWire Wire;')
    write('services/background_network.h','#include <stdint.h>\nnamespace services {namespace background_network {void requestPeripheralIo();bool peripheralIoReady();bool suspendForPeripheralIo(uint32_t);void resumeAfterPeripheralIo();}}')
    write('main.cpp',r'''
#include "services/camera_session.h"
#include <Arduino.h>
#include <Wire.h>
#include <cassert>
#include <string>
#include <vector>
Logger Serial;FakeWire Wire;
std::vector<std::string> trace;size_t kb=8000;bool uploadOk=true,agentOk=true,readOk=true,live=false;
std::vector<bool> starts;std::vector<int> sizes;
void Logger::println(const char*){}
void delay(unsigned ms){trace.push_back("delay"+std::to_string(ms));}
uint32_t millis(){return 100;}
namespace services {namespace background_network {
void requestPeripheralIo(){assert(false);}bool peripheralIoReady(){assert(false);return false;}
bool suspendForPeripheralIo(uint32_t ms){assert(ms==8000);trace.push_back("pause");return uploadOk;}
void resumeAfterPeripheralIo(){trace.push_back("resume");}
}}
namespace services {namespace agent_coordination {
bool TransportPause::pollAcquire(){assert(false);return false;}
bool TransportPause::acquire(uint32_t ms){if(held_)return true;assert(ms==8000);trace.push_back("agent-pause");return held_=agentOk;}
void TransportPause::release(){if(!held_)return;held_=false;trace.push_back("agent-resume");}
}}
namespace hal {
namespace wifi {void stop(){trace.push_back("wifi-stop");}void powerDown(){trace.push_back("wifi-off");}void restore(){}void scheduleRestore(){}}
namespace io {bool reinitAfterI2CRestart(){return true;}}
namespace camera {
bool init(int size,int format,const Tuning*t,int quality){assert(quality==-1);assert(format==4&&t->tag==42);sizes.push_back(size);trace.push_back("start"+std::to_string(size));live=starts.empty()?true:starts.front();if(!starts.empty())starts.erase(starts.begin());return live;}
void deinit(){trace.push_back("stop");live=false;}bool initialized(){return live;}
void settleAutoExposure(uint32_t ms){trace.push_back("settle"+std::to_string(ms));}
bool getFrame(CameraFrame*){trace.push_back("read");return readOk;}
void returnFrame(){trace.push_back("return");}
bool captureToFile(const char*){assert(false);return false;}
bool captureToSd(){assert(false);return false;}
const char* lastInitError(){return "failure";}
size_t availablePsramKb(){trace.push_back("memory");return kb;}
}}
void reset(){trace.clear();sizes.clear();starts.clear();kb=8000;uploadOk=agentOk=readOk=true;live=false;}
int main(){using namespace services::camera;using R=CaptureResolution;using S=Session::StillResult;
static_assert((int)R::Vga==0&&(int)R::Hd==1&&(int)R::FiveMp==2,"persisted choices");
for(auto n:{0u,259u,260u,1039u,1040u}){kb=n;assert(maxCaptureResolution()==(n<260?R::Vga:n<1040?R::Hd:R::FiveMp));}
hal::camera::Tuning tuning;
reset();{Session s;Session::FrameReader reader;
 assert(s.prepareCapture(false)==Session::PrepareResult::Ready);assert(s.start(8,4,&tuning));
 assert(reader.acquire(s));trace.clear();
 assert(s.prepareStill(R::Hd,tuning)==S::CameraBusy);assert(trace.empty());
 reader.close();assert(s.prepareStill(R::Vga,tuning)==S::Ready);
}
reset();{Session s;assert(s.start(8,4,&tuning));trace.clear();sizes.clear();assert(s.prepareStill(R::Hd,tuning)==S::Ready);assert((trace==std::vector<std::string>{"stop","delay80","pause","agent-pause","wifi-off","agent-resume","delay120","memory","start11","settle800"}));assert(s.backgroundHeld()&&s.running());}
reset();{Session s;assert(s.prepareStill(R::Vga,tuning)==S::Ready);assert((trace==std::vector<std::string>{"delay80","pause","agent-pause","wifi-stop","agent-resume","delay20","memory","start8","read","return"}));}
reset();starts={false,true};{Session s;assert(s.prepareStill(R::FiveMp,tuning)==S::Ready);assert((sizes==std::vector<int>{21,11}));assert(trace[trace.size()-1]=="settle1100");}
reset();kb=260;{Session s;assert(s.prepareStill(R::FiveMp,tuning)==S::Ready);assert((sizes==std::vector<int>{11}));assert(trace.back()=="settle1100");}
reset();kb=259;{Session s;assert(s.prepareStill(R::Hd,tuning)==S::Ready);assert((sizes==std::vector<int>{8}));assert(trace.back()=="settle800");}
reset();starts={false};{Session s;assert(s.prepareStill(R::Hd,tuning)==S::InitFailed);assert(sizes.size()==1&&!s.running());}
reset();starts={false,false};{Session s;assert(s.prepareStill(R::FiveMp,tuning)==S::InitFailed);assert(sizes.size()==2);}
reset();readOk=false;{Session s;assert(s.prepareStill(R::Vga,tuning)==S::WarmupFailed);assert(trace.back()=="read");assert(s.running());}
reset();uploadOk=false;{Session s;assert(s.prepareStill(R::Hd,tuning)==S::BackgroundBusy);assert(sizes.empty()&&!s.backgroundHeld());assert(trace.back()=="pause");}
reset();agentOk=false;{Session s;assert(s.prepareStill(R::Hd,tuning)==S::AgentBusy);assert(sizes.empty()&&!s.backgroundHeld());assert(trace.back()=="resume");}
}
''')
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(root),'-I'+str(ROOT/'include'),*[str(ROOT/'src/services'/name) for name in ['camera_capture.cpp','camera_session.cpp','peripheral_reservation.cpp']],str(root/'main.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
print('PASS still preparation ordering, PSRAM thresholds, resolution fallback, settling and resource/init/frame failures under sanitizers')
