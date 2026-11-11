"""Compile the production QR coordinator with faultable camera/worker/radio endpoints."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    def write(name, text):
        p = root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)
    write('services/camera_session.h', '''#pragma once
#include <stdint.h>
namespace hal { namespace camera {
struct Tuning { int sharpness,ae_level,gainceiling_idx; bool autofocus,singleShotAf; };
struct CameraFrame {};
} }
namespace services { namespace camera {
class Session {
public:
 enum class PrepareResult { Ready, BackgroundBusy, AgentBusy, Pending };
 using StageObserver=void(*)(const char*);
 PrepareResult prepareScan(StageObserver);PrepareResult pollPrepareScan(){return prepareScan(nullptr);}
 bool start(int,int,const hal::camera::Tuning*);void stop();bool running() const;
 void settle(uint32_t);bool read(hal::camera::CameraFrame*);void returnFrame();
 void releaseBackground();bool backgroundHeld() const;bool transportHeld() const;
 const char* lastError() const {return "camera failure";}
};
} }
''')
    write('hal/hal_wifi.h', '''#pragma once
#include <stdint.h>
namespace core {namespace wifi_qr {struct Credentials {int marker;};}}
namespace hal { namespace wifi {
enum class QrPhase { Preparing, Scanning, Connecting, Failed }; struct QrStatus {uint32_t id;QrPhase phase;};
uint32_t requestQrScan();QrStatus qrStatus();void releaseQrSession(uint32_t);
bool requestQrJoin(uint32_t,const core::wifi_qr::Credentials&,uint32_t);
} }
''')
    write('core/telemetry.h', '#define LOGE(...) ((void)0)\n#define LOGW(...) ((void)0)\n')
    write('Arduino.h', '#include <stdint.h>\nvoid delay(unsigned);\n')
    write('esp_camera.h', 'constexpr int FRAMESIZE_VGA=10, PIXFORMAT_JPEG=4;\n')
    write('main.cpp', r'''
#include "services/wifi_qr_session.h"
#include <cassert>
#include <string>
using S=services::wifi_qr::Session;
std::string trace;
bool reserve=true, live=false, init=true, held=false, stop=true, worker=true, start=true;
bool frame=true,busy=false,submit=true,borrowed=false,joinOk=true,cameraStops=true;
uint32_t timeout=999;
hal::wifi::QrStatus status{42,hal::wifi::QrPhase::Scanning};
void delay(unsigned ms){assert(ms==80);trace+='d';}
void observe(const char* s){trace+='[';trace+=s;trace+=']';}
namespace services { namespace camera {
Session::PrepareResult Session::prepareScan(StageObserver){trace+='P';held=reserve;return reserve?PrepareResult::Ready:PrepareResult::BackgroundBusy;}
bool Session::start(int size,int format,const hal::camera::Tuning* t){
 assert(size==10&&format==4&&t->sharpness==3&&t->autofocus&&t->singleShotAf&&t->ae_level==0&&t->gainceiling_idx==1);
 trace+='C';live=init;return live;
}
void Session::stop(){assert(!borrowed);trace+='S';if(cameraStops)live=false;}
bool Session::running() const{return live;}
void Session::settle(uint32_t ms){assert(ms==1000);trace+='E';}
bool Session::read(hal::camera::CameraFrame*){trace+='F';borrowed=frame;return frame;}
void Session::returnFrame(){assert(borrowed);trace+='R';borrowed=false;}
void Session::releaseBackground(){trace+='B';held=false;}
bool Session::backgroundHeld() const{return held;}
bool Session::transportHeld() const{return held;}
} }
namespace services { namespace qr {
bool shutdownWorker(uint32_t ms){trace+='X';timeout=ms;return stop;}
bool ensureWorker(){trace+='W';return start;}
const char* workerStartError(){return "Decoder unavailable";}
bool workerReady(){return worker;}
void logDiagnostics(){trace+='D';}
bool scanBusy(){return busy;}
bool submitForScanCenterCrop(int w,int h){assert(borrowed&&w==320&&h==240);trace+='Q';return submit;}
} }
namespace hal { namespace wifi {
uint32_t requestQrScan(){trace+='I';return 42;}
QrStatus qrStatus(){return status;}
void releaseQrSession(uint32_t id){assert(id==42&&stop&&!live);trace+='L';}
bool requestQrJoin(uint32_t id,const core::wifi_qr::Credentials& c,uint32_t now){
 assert(id==42&&c.marker==7&&now==123);trace+='J';return joinOk;
}
} }
void retireView(){assert(held);trace+='U';}
bool grayReady=true;
bool render(const hal::camera::CameraFrame&){assert(borrowed);trace+='V';return grayReady;}
int main(){
 S& s=S::instance();reserve=false;assert(!s.begin(observe)&&s.id()==0&&trace=="P");
 reserve=true;trace.clear();assert(s.begin(observe)&&s.id()==42&&trace=="PI");
 status.id=99;trace.clear();assert(s.prepareDecoder(observe)==S::Decoder::Waiting&&trace.empty());
 status.id=42;status.phase=hal::wifi::QrPhase::Preparing;
 assert(s.prepareDecoder(observe)==S::Decoder::Waiting&&trace.empty());
 status.phase=hal::wifi::QrPhase::Failed;
 assert(s.prepareDecoder(observe)==S::Decoder::RadioFailed&&trace.empty());
 status.phase=hal::wifi::QrPhase::Scanning;stop=false;
 assert(s.prepareDecoder(observe)==S::Decoder::Waiting&&trace=="X"&&timeout==0);
 stop=true;start=false;trace.clear();assert(s.prepareDecoder(observe)==S::Decoder::Failed);
 assert(std::string(s.decoderError())=="Decoder unavailable");
 assert(trace=="X[radio-stopped-before-worker]W[worker-failed]");
 start=true;trace.clear();assert(s.prepareDecoder(observe)==S::Decoder::Ready);
 assert(trace=="X[radio-stopped-before-worker]W[worker-ready-before-camera]");
 init=false;trace.clear();assert(!s.openCamera()&&trace=="C");
 init=true;trace.clear();assert(s.openCamera(S::Exposure::SettleDuringPreview)&&trace=="C");
 trace.clear();assert(s.openCamera()&&trace=="CE");
 worker=false;trace.clear();assert(s.renderFrame(render)==S::Frame::DecoderStopped&&trace.empty());
 worker=true;frame=false;assert(s.renderFrame(render)==S::Frame::Missing&&trace=="F");
 frame=true;trace.clear();assert(s.renderFrame(render)==S::Frame::Submitted&&trace=="FVQR"&&!borrowed);
 busy=true;trace.clear();assert(s.renderFrame(render)==S::Frame::Rendered&&trace=="FVR"&&!borrowed);
 busy=false;submit=false;trace.clear();assert(s.renderFrame(render)==S::Frame::Rendered&&trace=="FVQR"&&!borrowed);
 grayReady=false;busy=false;submit=true;trace.clear();assert(s.renderFrame(render)==S::Frame::Rendered&&trace=="FVR"&&!borrowed);
 trace.clear();s.stopCapture();assert(trace=="Sd");
 stop=false;trace.clear();assert(s.join({7},123)==S::Join::Waiting&&trace=="X");
 stop=true;joinOk=false;trace.clear();assert(s.join({7},123)==S::Join::Rejected&&trace=="XJ");
 joinOk=true;assert(s.join({7},123)==S::Join::Accepted);
 stop=false;trace.clear();s.fail();assert(trace=="DX"&&timeout==0);
 trace.clear();assert(!s.releaseStopped()&&trace=="X"&&held&&s.id()==42);
 live=true;cameraStops=false;trace.clear();assert(!s.releaseStopped()&&trace=="Sd"&&held);
 cameraStops=true;stop=true;trace.clear();assert(s.releaseStopped()&&trace=="SdXLB"&&!held&&!s.id());
 assert(s.renderFrame(render)==S::Frame::Idle);
 assert(s.begin(nullptr)&&s.openCamera());stop=false;trace.clear();
 assert(!s.close(retireView)&&trace=="SdDXU"&&timeout==3000&&held&&s.id()==42&&s.closing());
 trace.clear();assert(!s.begin(nullptr)&&trace=="X"&&s.id()==42&&held);
 trace.clear();s.pollCleanup();assert(trace=="X"&&held&&s.closing());
 stop=true;trace.clear();s.pollCleanup();assert(trace=="XLB"&&!held&&!s.id()&&!s.closing());
 trace.clear();s.pollCleanup();assert(trace.empty());
 assert(s.begin(nullptr)&&s.openCamera());cameraStops=false;trace.clear();
 assert(!s.close(retireView)&&held&&s.closing());
 trace.clear();s.pollCleanup();assert(trace=="Sd"&&held);
 cameraStops=true;trace.clear();s.pollCleanup();assert(trace=="SdXLB"&&!held&&!s.closing());
 assert(s.begin(nullptr)&&s.openCamera());trace.clear();assert(s.close(retireView));
 assert(trace=="SdDXULB"&&!held&&!s.closing());
 puts("PASS QR startup gates, tuning, borrowed frames, join outcomes, error cleanup and deferred exit cleanup/re-entry");
}
''')
    subprocess.run(['c++', '-std=c++11', '-fsanitize=address,undefined', '-I'+str(root), '-I'+str(ROOT/'include'), str(ROOT/'src/services/wifi_qr_session.cpp'), str(root/'main.cpp'), '-o', str(root/'test')], check=True)
    subprocess.run([str(root/'test')], check=True, timeout=10)
