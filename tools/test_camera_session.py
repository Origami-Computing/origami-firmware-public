"""Compile the real camera-session and reservation code against faultable HALs."""
from pathlib import Path
import shutil
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory() as directory:
        temp=Path(directory)
        def write(name,text):
            p=temp/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
        write('hal/hal_camera.h', '''#pragma once
#include <stdint.h>
namespace hal { namespace camera {
struct Tuning {}; struct CameraFrame {};
bool init(int,int,const Tuning*,int); void deinit(); bool initialized();
void settleAutoExposure(uint32_t); bool getFrame(CameraFrame*); void returnFrame();
bool captureToFile(const char*);bool captureToSd(); const char* lastInitError();
} }
''')
        write('Arduino.h','#include <stdint.h>\nvoid delay(unsigned);')
        write('esp_camera.h','constexpr int PIXFORMAT_JPEG=4;')
        write('hal/hal_wifi.h','namespace hal { namespace wifi { void stop(); void powerDown(); void restore(); void scheduleRestore(); } }')
        write('hal/hal_io.h','namespace hal { namespace io { bool reinitAfterI2CRestart(); } }')
        write('Pins.h','constexpr int SDA_PIN=1, SCL_PIN=2;')
        write('Wire.h','struct FakeWire { void end(); void begin(int,int); }; extern FakeWire Wire;')
        write('services/background_network.h','#include <stdint.h>\nnamespace services { namespace background_network { void requestPeripheralIo();bool peripheralIoReady();bool suspendForPeripheralIo(uint32_t); void resumeAfterPeripheralIo(); } }')
        write('main.cpp',r'''
#include "services/camera_session.h"
#include <cassert>
#include <string>
#include <Wire.h>
#include <thread>
#include <mutex>
#include <condition_variable>
std::string trace;
FakeWire Wire;
void FakeWire::end(){trace+='e';}
void FakeWire::begin(int,int){trace+='b';}
bool peripheralOk=true;bool captureOk=true;bool initOk=true, live=false, stopBlocked=false, agentOk=true, readOk=true, decoderOk=true;
int lastSize=0,lastFormat=0,lastQuality=-99;uint32_t settledMs=0;const hal::camera::Tuning* lastTuning=nullptr;
void delay(unsigned ms){assert(ms==50||ms==100);trace+=ms==50?'D':'d';}
bool prepareDecoder(){assert(live);trace+='B';return decoderOk;}
void closeDecoder(){trace+='b';}
bool prepareViewfinder(){if(!prepareDecoder())return false;trace+='O';return true;}
namespace services { namespace background_network {
void requestPeripheralIo(){assert(false);}bool peripheralIoReady(){assert(false);return false;}
bool suspendForPeripheralIo(uint32_t){trace+='P';return peripheralOk;}
void resumeAfterPeripheralIo(){trace+='p';}
} }
namespace services { namespace agent_coordination {
bool TransportPause::pollAcquire(){assert(false);return false;}
bool TransportPause::acquire(uint32_t){if(held_)return true;trace+='A';return held_=agentOk;}
void TransportPause::release(){if(!held_)return;held_=false;trace+='a';}
} }
namespace hal { namespace io { bool reinitAfterI2CRestart(){trace+='r';return true;} }
namespace wifi { void stop(){trace+='W';} void powerDown(){trace+='D';} void restore(){trace+='w';} void scheduleRestore(){trace+='d';} }
namespace camera {
bool init(int size,int format,const Tuning* tuning,int quality){lastQuality=quality;lastSize=size;lastFormat=format;lastTuning=tuning;trace+='I';live=initOk;return live;}
void deinit(){trace+='S';if(!stopBlocked)live=false;}
bool initialized(){return live;}
void settleAutoExposure(uint32_t ms){settledMs=ms;trace+='E';}
bool getFrame(CameraFrame*){trace+='G';return readOk;}
void returnFrame(){trace+='R';}
bool captureToFile(const char*){trace+='C';return true;}
bool captureToSd(){trace+='J';return captureOk;}
const char* lastInitError(){return "test failure";}
} }
int main(){
 using Session=services::camera::Session;
 using Result=Session::PrepareResult;
 hal::camera::CameraFrame frame;
 { Session unstarted;unstarted.restoreUi(Session::Resume::DeferredWithInputRecovery);assert(trace.empty()); }
 peripheralOk=false;
 { Session rejected;assert(rejected.prepareCapture(false)==Result::BackgroundBusy);
   rejected.restoreUi(Session::Resume::DeferredWithInputRecovery);assert(trace=="P"); }
 peripheralOk=true;trace.clear();
 // A frame-reader loan excludes all UI camera operations while its worker
 // retains a DMA frame. The worker is joined before the loan is returned.
 { Session session; Session::FrameReader reader, other;
   assert(!reader.acquire(session));
   assert(session.prepareCapture(false)==Result::Ready);
   assert(session.start());
   assert(session.read(&frame));assert(!reader.acquire(session));
   session.returnFrame();assert(reader.acquire(session));
   assert(!reader.acquire(session)&&!other.acquire(session));
   std::mutex mutex;std::condition_variable cv;bool borrowed=false,release=false;
   std::thread worker([&]{
     assert(reader.read(&frame));assert(!reader.read(&frame));
     std::unique_lock<std::mutex> lock(mutex);borrowed=true;cv.notify_one();
     cv.wait(lock,[&]{return release;});lock.unlock();reader.returnFrame();
   });
   {std::unique_lock<std::mutex> lock(mutex);cv.wait(lock,[&]{return borrowed;});}
   trace.clear();
   assert(!session.read(&frame));session.returnFrame();session.stop();
   session.releaseBackground();session.restoreUi(Session::Resume::Immediate);
   session.settle(100);assert(!session.start());
   assert(!session.captureToFile("/unused")&&!session.captureDiagnosticToSd());
   assert(session.prepareCapture(false)==Result::CameraBusy);
   hal::camera::Tuning tuning;
   assert(!session.openPreview(8,tuning,prepareViewfinder));
   assert(!session.restartPreview(8,tuning,500,prepareViewfinder));
   assert(trace.empty()&&session.running()&&session.backgroundHeld());
   {std::lock_guard<std::mutex> lock(mutex);release=true;}cv.notify_one();worker.join();
   assert(trace=="R");reader.close();reader.close();
   assert(!reader.read(&frame));assert(session.read(&frame));session.returnFrame();
   assert(reader.acquire(session));assert(reader.read(&frame));reader.close();
   session.stop();session.releaseBackground();
 }
 trace.clear();
 { Session s; assert(!s.read(&frame));
   assert(s.prepareCapture(false)==Result::Ready);assert(trace=="PAWa");
   assert(s.backgroundHeld()&&!s.transportHeld());
   assert(s.start(8,4,nullptr));assert(s.read(&frame));s.returnFrame();
   s.stop();s.releaseBackground();assert(trace=="PAWaIGRSp"); }
 trace.clear();
 { Session s;assert(s.prepareScan()==Result::Ready);assert(s.transportHeld());
   s.releaseBackground();assert(trace=="PAap"); }
 trace.clear();
 { Session s;assert(s.prepareCapture(true)==Result::Ready);assert(trace=="PADa"); }
 assert(trace=="PADap");
 trace.clear();agentOk=false;
 { Session s;assert(s.prepareScan()==Result::AgentBusy);assert(!s.backgroundHeld());
   s.restoreUi(Session::Resume::Immediate);assert(trace=="PAp"); }
 assert(trace=="PAp");agentOk=true;trace.clear();initOk=false;
 { Session s;assert(!s.start(8,4,nullptr));assert(!s.running());s.stop();assert(trace=="IS"); }
 trace.clear();initOk=true;
 { Session s;assert(s.start(8,4,nullptr));stopBlocked=true;s.stop();assert(s.running());
   stopBlocked=false;s.stop();assert(!s.running());assert(trace=="ISS"); }
 trace.clear();
 { Session s;s.prepareScan();s.start(8,4,nullptr); }
 assert(trace=="PAISap"); // Driver closes before transport/workers resume.
 trace.clear();
 { Session s;s.prepareCapture(false);s.start(8,4,nullptr);s.stop();
   s.restoreUi(Session::Resume::DeferredWithInputRecovery); }
 assert(trace=="PAWaISdpebr");
 // Settings restores the radio before final app exit. Input recovery remains
 // owed exactly once, without a second radio restore or any new reservation.
 trace.clear();
 { Session s;s.prepareCapture(false);s.start(8,4,nullptr);s.stop();
   s.restoreUi(Session::Resume::Immediate);assert(trace=="PAWaISwp");
   s.restoreUi(Session::Resume::DeferredWithInputRecovery);assert(trace=="PAWaISwpebr");
   s.restoreUi(Session::Resume::DeferredWithInputRecovery);assert(trace=="PAWaISwpebr"); }
 // Initial entry waits, gives DMA priority, enables the UI before settling,
 // and never consumes a warmup frame. Failures stop before settling/rendering.
 // Explicit compression survives both entry and restart; default callers retain -1.
 { Session s;hal::camera::Tuning tuning;
   assert(s.openPreview(8,tuning,prepareViewfinder,12)&&lastQuality==12);
   assert(s.restartPreview(8,tuning,500,prepareDecoder,12)&&lastQuality==12);
   assert(s.restartPreview(5,tuning,500,prepareDecoder)&&lastQuality==-1);
   assert(s.restartPreview(5,tuning,500,prepareDecoder,-1,0)&&lastFormat==0);
   assert(s.restartPreview(8,tuning,500,prepareDecoder,12)&&lastFormat==4);
 }
 hal::camera::Tuning initialTuning;
 trace.clear();
 { Session s;assert(s.openPreview(8,initialTuning,prepareViewfinder));
   assert(trace=="dIBOE"&&settledMs==700&&lastTuning==&initialTuning); }
 trace.clear();initOk=false;
 { Session s;assert(!s.openPreview(8,initialTuning,prepareViewfinder));
   assert(trace=="dIS"&&!s.running()); }
 trace.clear();initOk=true;decoderOk=false;
 { Session s;assert(!s.openPreview(8,initialTuning,prepareViewfinder));
   assert(trace=="dIBS"&&!s.running()); }
 decoderOk=true;
 // Preview recovery preserves the reservation and allocates decoder after DMA.
 hal::camera::Tuning tuning;
 trace.clear();
 { Session s;s.prepareCapture(false);s.start(8,4,&tuning);trace.clear();
   closeDecoder();assert(s.restartPreview(8,tuning,500,prepareDecoder));
   assert(trace=="bSDIBEGR");assert(s.running()&&s.backgroundHeld());
   assert(lastSize==8&&lastFormat==4&&lastTuning==&tuning&&settledMs==500); }
 trace.clear();
 { Session s;s.start(8,4,&tuning);trace.clear();initOk=false;
   closeDecoder();assert(!s.restartPreview(8,tuning,400,prepareDecoder));
   assert(trace=="bSDIS");assert(!s.running()); }
 initOk=true;trace.clear();decoderOk=false;
 { Session s;s.start(8,4,&tuning);trace.clear();
   closeDecoder();assert(!s.restartPreview(5,tuning,500,prepareDecoder));
   assert(trace=="bSDIBS");assert(!s.running());assert(lastSize==5); }
 decoderOk=true;trace.clear();readOk=false;
 { Session s;s.start(8,4,&tuning);trace.clear();
   closeDecoder();assert(s.restartPreview(8,tuning,500,prepareDecoder));
   assert(trace=="bSDIBEG");assert(s.running()); } // Missing warmup stays non-fatal.
 readOk=true;
 trace.clear();
 { Session s;assert(!s.captureDiagnosticToSd());assert(trace.empty());
   assert(s.start());assert(lastSize==-1&&lastFormat==-1&&lastTuning==nullptr);
   assert(!s.backgroundHeld()&&!s.transportHeld());
   assert(s.captureDiagnosticToSd());captureOk=false;assert(!s.captureDiagnosticToSd());
   assert(trace=="IJJ");s.stop();assert(!s.captureDiagnosticToSd());assert(trace=="IJJS"); }
 captureOk=true;
 // An old session may have stopped DMA while its decoder is still draining.
 // Its reservation must exclude a replacement until explicit release.
 trace.clear();
 { Session first, next;
   assert(first.prepareScan()==Result::Ready);assert(first.start());first.stop();
   trace.clear();assert(next.prepareCapture(false)!=Result::Ready);
   assert(!next.start());next.stop();next.restoreUi(Session::Resume::DeferredWithInputRecovery);
   assert(trace.empty()); // No driver, radio restore, or I2C reset by rejected owner.
   first.releaseBackground();trace.clear();
   assert(next.prepareCapture(false)==Result::Ready);assert(next.start());
   assert(trace=="PAWaI");
 }
 trace.clear();
 { Session first, next;
   assert(first.start());stopBlocked=true;first.stop();trace.clear();
   assert(!next.start());next.stop();assert(trace.empty());
   stopBlocked=false;first.stop();assert(next.start());
 }
 trace.clear();peripheralOk=false;
 { Session failed, next;
   assert(failed.prepareScan()==Result::BackgroundBusy);peripheralOk=true;
   assert(next.prepareScan()==Result::Ready);
 }
 trace.clear();agentOk=false;
 { Session failed, next;
   assert(failed.prepareScan()==Result::AgentBusy);agentOk=true;
   assert(next.prepareScan()==Result::Ready); // Failed preparation releases ownership.
 }

}
''')
        subprocess.run([shutil.which('c++') or 'c++','-std=c++11','-pthread','-fsanitize=address,undefined','-I'+str(temp),'-I'+str(ROOT/'include'),
                        str(ROOT/'src/services/camera_session.cpp'),str(ROOT/'src/services/camera_preview_session.cpp'),str(ROOT/'src/services/peripheral_reservation.cpp'),
                        str(temp/'main.cpp'),'-o',str(temp/'check')],check=True)
        subprocess.run([str(temp/'check')],check=True,timeout=10)
    print('PASS camera radio/reservation policies, preview allocation order, init/decoder/warmup failures and blocked-stop retry under sanitizers')


if __name__=='__main__':main()
