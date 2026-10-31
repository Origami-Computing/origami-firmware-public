"""Fault-inject the real upload coordinator's suspension/restore contracts."""
from pathlib import Path
import subprocess,tempfile,sys
ROOT=Path(__file__).resolve().parents[1]
assert 'services::uploads::tick();\n                services::background_network::tick();' in (ROOT/'src/services.cpp').read_text()
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    def write(name,text):
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    write('Arduino.h', '#pragma once\n#include <stdint.h>\n#include "upload_queue.h"\nuint32_t millis();')
    write('freertos/FreeRTOS.h', '#pragma once\n#define pdMS_TO_TICKS(x) (x)')
    write('freertos/task.h', '#pragma once\n#include <stdint.h>\nusing TaskHandle_t=void*;\nTaskHandle_t xTaskGetCurrentTaskHandle();\nvoid vTaskDelay(uint32_t);')
    write('core/watchdog.h', 'namespace core {namespace watchdog {void feed();}}')
    write('upload_queue.h',r'''#pragma once
#include <stdint.h>
struct Logger {void println(const char*){} template<class... T> void printf(const char*,T...) {}};extern Logger Serial;
extern int queueTicks;
struct UploadQueue {
 void setNetworkCoordinator(bool(*)(uint32_t),void(*)());
 bool init();void tick(){++queueTicks;}bool workerRunning();void requestStopForVoice();bool stopWorkerForVoice(uint32_t);bool resumeWorker();
};''')
    write('services/morning_quote.h','#include <stdint.h>\nnamespace services {namespace morning_quote {bool pollPeripheralPause();bool suspendForPeripheralIo(uint32_t);void resumeAfterPeripheralIo();}}')
    write('core/menu_config.h','')
    write('services/net_poll.h','#include <stdint.h>\nnamespace services {namespace net_poll {bool onboardingRegistrationPriority();void requestStop();bool running();bool stop(uint32_t);bool start();void requestStart();bool startRequested();}}')
    write('hal/hal_sd.h','namespace hal {namespace sd {bool mounted();}}')
    write('widgets/sync.h','#include <stdint.h>\nnamespace widgets {namespace sync {void requestPollingPause();bool pollingPaused();bool suspendPolling(uint32_t);void resumePolling();}}')
    write('services/firmware_update.h','namespace services {namespace firmware_update {inline bool noticeInFlight(){return false;}}}')
    write('main.cpp',r'''
#include "services/uploads.h"
#include "services/background_network.h"
#include "services/agent_coordination.h"
#include "services/peripheral_reservation.h"
#include <cassert>
#include "hal/audio_retirement.h"
#include <string>
#include <atomic>
#include <thread>
#include <chrono>
namespace agent=services::agent_coordination;
uint32_t now=0;bool acknowledge=true;
int queueTicks=0;
bool concurrentInit=false,failFirstInit=false;
std::atomic<int> initAttempts{0},initializers{0},initWaiters{0},installs{0};
std::atomic<bool> finishInit{false};
int initializedPayload=0;
uint32_t millis(){return now;}
void* xTaskGetCurrentTaskHandle(){return nullptr;}
void vTaskDelay(uint32_t ms){
 if(concurrentInit){
  thread_local bool waiting=false;
  if(!waiting){++initWaiters;waiting=true;}
  std::this_thread::sleep_for(std::chrono::milliseconds(1));return;
 }
 now+=ms;if(acknowledge&&agent::suspendRequested())agent::acknowledgeSuspended();
}
bool (*acquireNetwork)(uint32_t)=nullptr;void (*releaseNetwork)()=nullptr;
void UploadQueue::setNetworkCoordinator(bool(*acquire)(uint32_t),void(*release)()){
 ++installs;acquireNetwork=acquire;releaseNetwork=release;
}
namespace core {namespace watchdog {void feed(){}}}
Logger Serial;std::string trace;bool initOk=true,mounted=false,stopOk=true,netOk=true,widgetOk=true,quoteOk=true,worker=false;
bool UploadQueue::init(){
 if(!concurrentInit){trace+='i';return initOk;}
 assert(++initializers==1); // No second initializer may enter storage/allocation.
 const int attempt=++initAttempts;
 while(!finishInit.load())std::this_thread::yield();
 const bool ok=!(failFirstInit&&attempt==1);
 if(ok)initializedPayload=42; // Publication must include initialized queue state.
 --initializers;return ok;
}
bool UploadQueue::workerRunning(){return worker;}
void UploadQueue::requestStopForVoice(){trace+='r';}
bool UploadQueue::stopWorkerForVoice(uint32_t){
 trace+='u';
 // Model the production worker's release callback before its completion flag.
 // This checks service integration, not FreeRTOS scheduling or queue atomics.
 if(stopOk&&worker){releaseNetwork();assert(!agent::suspendRequested());worker=false;}
 return stopOk;
}
bool UploadQueue::resumeWorker(){trace+='U';return true;}
namespace hal {namespace sd {bool mounted(){return ::mounted;}}}
namespace services {namespace net_poll {bool onboardingRegistrationPriority(){return false;}void requestStop(){trace+='s';}bool running(){return !netOk;}bool pending=false;void requestStart(){pending=true;}bool stop(uint32_t){pending=false;trace+='n';return netOk;}bool start(){trace+='N';return true;}bool startRequested(){if(!pending)return false;pending=false;return start();}}}
namespace services {
namespace morning_quote {bool pollPeripheralPause(){return quoteOk;}bool suspendForPeripheralIo(uint32_t){trace+='q';return quoteOk;}void resumeAfterPeripheralIo(){trace+='Q';}}
}
namespace widgets {namespace sync {void requestPollingPause(){trace+='w';}bool pollingPaused(){return widgetOk;}bool suspendPolling(uint32_t){trace+='w';return widgetOk;}void resumePolling(){trace+='W';}}}
void serviceTick(){services::uploads::tick();services::background_network::tick();}
int main(int argc,char** argv){using namespace services::uploads;using namespace services::background_network;
 if(argc>1&&std::string(argv[1]).find("init-")==0){
  concurrentInit=true;failFirstInit=std::string(argv[1])=="init-retry";mounted=true;
  bool firstReady=false,secondReady=false;
  std::atomic<bool> secondDone{false},queueDone{false};
  std::thread first([&]{firstReady=ensureInit();});
  while(initAttempts.load()==0)std::this_thread::yield();
  std::thread second([&]{secondReady=ensureInit();assert(initializedPayload==42);secondDone=true;});
  std::thread reader([&]{(void)queue();assert(initializedPayload==42);queueDone=true;});
  const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);
  while(initWaiters.load()<2){
   assert(std::chrono::steady_clock::now()<deadline);std::this_thread::yield();
  }
  assert(!secondDone.load()&&!queueDone.load()&&initAttempts.load()==1);
  assert(readyQueue()==nullptr); // Never publish a partially initialized singleton.
  finishInit=true;first.join();second.join();reader.join();
  assert(firstReady==!failFirstInit&&secondReady&&queueDone.load());
  assert(initAttempts.load()==(failFirstInit?2:1));
  assert(installs.load()==initAttempts.load());
  assert(ensureInit()&&initializedPayload==42);assert(readyQueue()==&queue());
  assert(installs.load()==initAttempts.load()); // Published ready path does no setup.
  return 0;
 }
 if(argc>1){
  mounted=true;assert(ensureInit());trace.clear();
  if(std::string(argv[1])=="http-admission"){
   PeriodicWork notice;assert(notice.tryEnter());
   assert(!acquireNetwork(30)); // Notice excludes upload; rollback releases Agent.
   assert(!agent::suspendRequested());
   notice.release();assert(acquireNetwork(30));
   assert(!notice.tryEnter()); // Upload excludes notice before worker allocation.
   releaseNetwork();assert(notice.tryEnter());notice.release();
   assert(acquireNetwork(30));releaseNetwork(); // Slot remains reusable.
  }else if(std::string(argv[1])=="handoff"){
   using Lease=services::background_network::PeripheralReservation;
   using Result=Lease::Result;
   agent::workerStarted();
   // Uploads own the first pause. Peripheral acquisition drains their release
   // before asking for its own acknowledgment; no later upload resume remains.
   assert(acquireNetwork(30));worker=true;assert(agent::suspendRequested());
   Lease foreground;assert(foreground.acquire(true,30)==Result::Acquired);
   assert(!worker&&agent::suspendRequested());
   {services::background_network::ServiceWork blocked;assert(!blocked);}
   foreground.release();assert(!agent::suspendRequested());
   // An upload stop timeout must not enter or clear the upload's Agent pause.
   assert(acquireNetwork(30));worker=true;stopOk=false;
   assert(foreground.acquire(true,30)==Result::PeripheralBusy);
   assert(agent::suspendRequested());foreground.release();assert(agent::suspendRequested());
   stopOk=true;releaseNetwork();worker=false;
   assert(!agent::suspendRequested());
  }else if(std::string(argv[1])=="overlap"){
   // Distinct reservations retain Agent pause until the final owner releases.
   using Lease=services::background_network::PeripheralReservation;
   using Result=Lease::Result;
   agent::workerStarted();
   Lease first,second;
   assert(first.acquire(true,30)==Result::Acquired);
   assert(second.acquire(true,30)==Result::Acquired);
   first.release();
   assert(second.held()&&agent::suspendRequested());
   second.release();
   assert(!agent::suspendRequested());
   assert(first.acquire(true,30)==Result::Acquired);acknowledge=false;
   // Already-acknowledged owners share the cleanup guarantee without re-waiting.
   assert(second.acquire(true,0)==Result::Acquired);
   second.release();assert(first.held()&&agent::suspendRequested());
   first.release();assert(!agent::suspendRequested());
  }else if(std::string(argv[1])=="admission"){
   {services::background_network::ServiceWork active;assert(active);
   // No worker exists yet, but a service iteration may be acquiring networking.
   assert(!suspendForPeripheralIo(30));
   assert(trace=="qQ");}
   assert(suspendForPeripheralIo(30));resumeAfterPeripheralIo();
   services::background_network::ServiceWork recovered;assert(recovered);
  }else{
   assert(suspendForPeripheralIo(30));trace.clear();
   assert(suspendForPeripheralIo(30));assert(trace.empty());
   resumeAfterPeripheralIo();assert(trace.empty());
   services::background_network::ServiceWork blocked;assert(!blocked);
   services::background_network::RadioPause radio;
   assert(radio.acquire(30)==services::background_network::RadioPause::Result::AlreadyOwned);
   assert(trace.empty());
   resumeAfterPeripheralIo();
   {services::background_network::ServiceWork admitted;assert(admitted);}
   trace.clear();resumeAfterPeripheralIo();assert(trace.empty());
   assert(radio.acquire(30)==services::background_network::RadioPause::Result::Acquired);
   trace.clear();assert(!suspendForPeripheralIo(30));assert(trace.empty());
   radio.release();
   services::background_network::ServiceWork admitted;assert(admitted);
  }
  return 0;
 }

 // Optional absent storage must not prevent voice; audio retirement still does.
 {hal::audio::RetirementBlock retiring;retiring.acquire();
 auto voice=suspendForKamiVoice();auto music=suspendForMusicAudio();
 assert(!voice.uploadsReady && !music.uploadsReady);
 assert(voice.poller==AudioSuspension::Poller::NotRequested && music.poller==AudioSuspension::Poller::NotRequested);
 assert(!suspendForPeripheralIo(30) && trace.empty());}
 assert(readyQueue()==nullptr);
 auto idle=suspendForMusicAudio();assert(idle.uploadsReady&&idle.poller==AudioSuspension::Poller::NotRequested);assert(trace.empty());
 auto absent=suspendForKamiVoice();assert(absent.uploadsReady&&absent.poller==AudioSuspension::Poller::StopPending);
 assert(trace=="sw"&&voicePriorityActive());trace.clear();resumeAfterKamiVoice();assert(trace=="W");trace.clear();serviceTick();assert(trace=="N");trace.clear();
 // Voice must never initialize SD or synchronously wait for existing workers.
 mounted=true;initOk=false;assert(suspendForKamiVoice().uploadsReady);assert(trace=="sw");
 assert(readyQueue()==nullptr);trace.clear();resumeAfterKamiVoice();trace.clear();
 mounted=false;initOk=true;assert(pauseWorkerForRadio(30));assert(trace.empty());
 resumeAfterRadioPause();serviceTick();assert(trace=="N");trace.clear();
 mounted=true;assert(ensureInit());assert(trace=="i");trace.clear();
 mounted=false;stopOk=false;worker=true;
 assert(suspendForKamiVoice().uploadsReady);assert(trace=="rsw");trace.clear();
 assert(!kamiVoiceNetworkReady());worker=false;netOk=false;assert(!kamiVoiceNetworkReady());
 netOk=true;widgetOk=false;assert(!kamiVoiceNetworkReady());widgetOk=true;assert(kamiVoiceNetworkReady());
 trace.clear();resumeAfterKamiVoice();assert(trace=="WU");trace.clear();
 // Alert audio uses independent ownership: either release order keeps the
 // other foreground owner protected. No worker admission while ringing.
 suspendForAlertAudio();assert(audioPriorityActive()&&!voicePriorityActive());
 assert(trace=="rsw");trace.clear();
 {PeriodicWork periodic;assert(!periodic.tryEnter());}
 assert(!acquireUploadNetwork(30));int priorTicks=queueTicks;serviceTick();assert(trace.empty()&&queueTicks==priorTicks);
 assert(suspendForKamiVoice().uploadsReady);trace.clear();
 resumeAfterAlertAudio();assert(audioPriorityActive()&&voicePriorityActive()&&trace.empty());
 resumeAfterKamiVoice();assert(!audioPriorityActive()&&trace=="WU");trace.clear();
 suspendForAlertAudio();suspendForKamiVoice();trace.clear();
 resumeAfterKamiVoice();assert(audioPriorityActive()&&!voicePriorityActive()&&trace.empty());
 resumeAfterMusicAudio();resumeAfterRadioPause();serviceTick();assert(trace.empty());
 resumeAfterAlertAudio();assert(!audioPriorityActive()&&trace=="WU");trace.clear();
 resumeAfterAlertAudio();assert(trace.empty());
 {PeriodicWork periodic;assert(periodic.tryEnter());}
 mounted=true;assert(!suspendForMusicAudio().uploadsReady);assert(trace=="u");trace.clear();stopOk=true;
 netOk=false;auto pending=suspendForMusicAudio();assert(pending.uploadsReady&&pending.poller==AudioSuspension::Poller::StopPending);trace.clear();
 resumeAfterMusicAudio();assert(trace=="U");trace.clear();serviceTick();assert(trace=="N");trace.clear();
 // Peripherals require the stronger guarantee and roll back on timeout.
 quoteOk=false;assert(!suspendForPeripheralIo());assert(trace=="q");trace.clear();quoteOk=true;
 stopOk=false;assert(!suspendForPeripheralIo());assert(trace=="quQ");trace.clear();stopOk=true;
 assert(!suspendForPeripheralIo());assert(trace=="qunUQ");trace.clear();serviceTick();assert(trace=="N");trace.clear();
 netOk=true;widgetOk=false;assert(!suspendForPeripheralIo());assert(trace=="qunwUQ");trace.clear();
 widgetOk=true;assert(suspendForPeripheralIo());assert(trace=="qunw");trace.clear();
 {hal::audio::RetirementBlock retiring;retiring.acquire();
 assert(!suspendForPeripheralIo() && !suspendForMusicAudio().uploadsReady && !suspendForKamiVoice().uploadsReady);
 assert(trace.empty());} // Refused nested acquisition must not increment holders or touch workers.

 resumeAfterPeripheralIo();assert(trace=="WUQ");trace.clear();serviceTick();assert(trace=="N");
 trace.clear();assert(suspendForPeripheralIo());trace.clear();
 suspendForAlertAudio();trace.clear();resumeAfterPeripheralIo();
 assert(trace=="Q"&&audioPriorityActive());trace.clear();
 resumeAfterAlertAudio();assert(trace=="WU");trace.clear();serviceTick();assert(trace=="N");
 trace.clear();assert(suspendForPeripheralIo());trace.clear();
 suspendForAlertAudio();trace.clear();resumeAfterAlertAudio();
 assert(trace.empty()&&!audioPriorityActive());
 resumeAfterPeripheralIo();assert(trace=="WUQ");trace.clear();serviceTick();assert(trace=="N");
}
''')
    subprocess.run(['c++','-std=c++11','-pthread','-fsanitize=address,undefined','-I'+str(root),'-I'+str(ROOT/'include'),str(ROOT/'src/services/uploads.cpp'),str(ROOT/'src/services/background_network.cpp'),str(ROOT/'src/services/agent_coordination.cpp'),str(ROOT/'src/services/peripheral_reservation.cpp'),str(root/'main.cpp'),str(ROOT/'src/hal/audio_retirement.cpp'),'-o',str(root/'test')],check=True)
    for args in ([sys.argv[1:]] if len(sys.argv)>1 else [[],['http-admission'],['admission'],['nested'],['handoff'],['overlap'],['init-success'],['init-retry']]):
        subprocess.run([str(root/'test')]+args,check=True,timeout=20)
print('PASS audio/peripheral contracts, admission/rollback, shared I/O ownership, radio exclusion, deferred restart, Agent handoff, overlapping reservations and concurrent init/publication/retry under sanitizers')
