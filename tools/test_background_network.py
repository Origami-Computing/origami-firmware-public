"""Fault-inject real radio handoff and svc_task admission/rollback semantics."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
bootstrap = (ROOT/'src/services.cpp').read_text()
bootstrap = bootstrap[bootstrap.index('bool startBackgroundTasks() {'):].split('\n}  // namespace services',1)[0]

with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    def write(name, text):
        path = root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    write('Arduino.h', '#pragma once\n#include <stdint.h>\nstruct Logger {void println(const char*){}};extern Logger Serial;\nuint32_t millis();')
    write('freertos/FreeRTOS.h', '#pragma once\n#define pdMS_TO_TICKS(x) (x)')
    write('freertos/task.h', '#pragma once\n#include <stdint.h>\nvoid vTaskDelay(uint32_t);')
    write('hal/hal_sd.h', 'namespace hal {namespace sd {inline bool mounted(){return true;}}}')
    write('core/watchdog.h', 'namespace core {namespace watchdog {void feed();}}')
    write('services/net_poll.h', '#include <stdint.h>\nnamespace services {namespace net_poll {bool onboardingRegistrationPriority();void requestStop();bool running();bool stop(uint32_t);bool start();void requestStart();bool startRequested();}}')
    write('services/uploads.h', '#pragma once\n#include <stdint.h>\nstruct UploadQueue {void requestStopForVoice();bool stopWorkerForVoice(uint32_t);bool resumeWorker();bool workerRunning();};\nnamespace services {namespace uploads {UploadQueue* readyQueue();UploadQueue& queue();bool ensureInit();}}')
    write('services/morning_quote.h', '#include <stdint.h>\nnamespace services {namespace morning_quote {bool pollPeripheralPause();bool suspendForPeripheralIo(uint32_t);void resumeAfterPeripheralIo();}}')
    write('widgets/sync.h', '#include <stdint.h>\nnamespace widgets {namespace sync {void requestPollingPause();bool pollingPaused();bool suspendPolling(uint32_t);void resumePolling();}}')
    write('services/firmware_update.h','namespace services {namespace firmware_update {bool noticeActive=false;inline bool noticeInFlight(){return noticeActive;}}}')
    write('main.cpp', r'''
#include "services/background_network.h"
namespace services {namespace firmware_update {extern bool noticeActive;}}
#include "services/uploads.h"
#include "Arduino.h"
#include <cassert>
#include "hal/audio_retirement.h"
#include <functional>
#include <memory>
#include <string>
using namespace services::background_network;
using Result=RadioPause::Result;
Logger Serial;UploadQueue queueInstance;
uint32_t now=0;int failure=0,feeds=0,starts=0,requests=0;
bool onboardingPriority=false,asyncMode=false,workerLive=false,pollerLive=false,widgetAck=true;
bool quote=false,upload=false,netStop=false,widget=false,agent=false,resumePending=false;
std::function<void()> delayed;
std::string trace;
uint32_t millis(){return now;}
void vTaskDelay(uint32_t ms){now+=ms;if(delayed)delayed();}
void closed(){ServiceWork work;assert(!work);}
namespace core {
namespace watchdog {void feed(){++feeds;}}
}
namespace services {
namespace net_poll {bool onboardingRegistrationPriority(){return onboardingPriority;}void requestStop(){if(asyncMode)netStop=true;}bool running(){return pollerLive;}void requestStart(){++requests;}bool startRequested(){++starts;return true;}bool stop(uint32_t ms){assert(ms==30);closed();trace+='n';netStop=true;return failure!=3;}bool start(){++starts;return true;}}
namespace morning_quote {
bool pollPeripheralPause(){quote=true;return failure!=1;}
bool suspendForPeripheralIo(uint32_t ms){assert(ms==30);trace+='q';quote=failure!=1;return quote;}
void resumeAfterPeripheralIo(){assert(quote);trace+='Q';quote=false;}
}
namespace uploads {
UploadQueue* readyQueue(){return &queueInstance;}UploadQueue& queue(){return queueInstance;}bool ensureInit(){return true;}
}

namespace agent_coordination {
bool isSessionActive(){return false;}
bool TransportPause::acquire(uint32_t ms){if(held_)return true;assert(ms==30);closed();trace+='a';agent=failure!=5;return held_=agent;}
void TransportPause::release(){if(!held_)return;held_=false;closed();assert(agent);trace+='A';agent=false;}
}
}
void UploadQueue::requestStopForVoice(){if(asyncMode)upload=true;}
bool UploadQueue::stopWorkerForVoice(uint32_t ms){assert(ms==30);closed();trace+='u';upload=failure!=2;return upload;}
bool UploadQueue::resumeWorker(){closed();trace+='U';upload=false;resumePending=true;return true;}
bool UploadQueue::workerRunning(){return workerLive;}

namespace widgets {namespace sync {
void requestPollingPause(){if(asyncMode)widget=true;}bool pollingPaused(){return widgetAck;}
bool suspendPolling(uint32_t ms){assert(ms==30);closed();trace+='w';widget=failure!=4;return widget;}
void resumePolling(){closed();assert(widget);trace+='W';widget=false;}
}}
void restored(){
 assert(!quote&&!upload&&!widget&&!agent);
 ServiceWork work;assert(work);
 // This represents the deferred poller restart in the next admitted iteration.
 if(resumePending){netStop=false;resumePending=false;}
 assert(!netStop);
}
namespace hal {namespace wifi {void startTask(){}}
namespace tasks {enum class Id{Services};void* spawn(Id,void(*)(void*)){return reinterpret_cast<void*>(1);}}}
namespace widgets {namespace sync {void startPolling(){}}}
namespace services {void svcTask(void*){}
''' + bootstrap + r'''
}
int main(){
 {RadioPause pause;assert(pause.acquire(30)==Result::Acquired);
  starts=requests=0;
  assert(services::startBackgroundTasks());
  assert(starts==0 && requests==1); // Bootstrap must not steal the reserved stack.
  {ServiceWork blocked;assert(!blocked);}
  pause.release();
  {ServiceWork admitted;assert(admitted);tick();}
  assert(starts==1);
  restored();trace.clear();
 }

 assert(kamiVoiceNetworkReady());onboardingPriority=true;assert(!kamiVoiceNetworkReady());onboardingPriority=false;assert(kamiVoiceNetworkReady());
 { PeriodicWork first,second;assert(first.tryEnter());assert(first.tryEnter());assert(!second.tryEnter());first.release();assert(second.tryEnter()); }
 { PeriodicWork after;assert(after.tryEnter());after.yieldIfContended(); }
 { PeriodicWork owner,waiter;assert(owner.tryEnter());assert(!waiter.tryEnter());
   assert(PeriodicWork::hasWaitingPeer());assert(owner.yieldIfContended());
   assert(!PeriodicWork::hasWaitingPeer());assert(waiter.tryEnter());
   assert(!waiter.yieldIfContended()); }

 {services::firmware_update::noticeActive=true;RadioPause denied;
 assert(denied.acquire(30)==Result::ServiceBusy && trace.empty());
 denied.release();assert(trace.empty());
 services::firmware_update::noticeActive=false;restored();}
 {hal::audio::RetirementBlock retiring;retiring.acquire();RadioPause denied;
 assert(denied.acquire(30)==Result::PlaybackBusy && trace.empty());
 assert(std::string(resultName(Result::PlaybackBusy))=="playback-retiring");
 denied.release();assert(trace.empty());}

 {ServiceWork first;assert(first);ServiceWork duplicate;assert(!duplicate);}
 const Result failures[]={Result::Acquired,Result::QuoteBusy,Result::UploadBusy,Result::PollerBusy,Result::WidgetBusy,Result::AgentBusy};
 for(failure=1;failure<=5;++failure){
  RadioPause pause;assert(pause.acquire(30)==failures[failure]);restored();
  auto previous=trace;pause.release();assert(trace==previous);
 }
 failure=0;
 {
  ServiceWork active;
  RadioPause pause;
  assert(pause.acquire(30)==Result::ServiceBusy);
  assert(!quote&&!upload&&!netStop);
  ServiceWork stillBusy;assert(!stillBusy); // rollback cannot clear active work.
 }
 restored();
 // Existing work finishes after admission closes; no new iteration can enter.
 auto active=std::unique_ptr<ServiceWork>(new ServiceWork);
 delayed=[&]{closed();active.reset();};
 {
  RadioPause pause;assert(pause.acquire(30)==Result::Acquired);delayed=nullptr;
  assert(quote&&upload&&netStop&&widget&&agent);closed();
  auto previous=trace;assert(pause.acquire(30)==Result::Acquired);assert(trace==previous);
  {RadioPause other;assert(other.acquire(30)==Result::AlreadyOwned);}
  assert(trace==previous);closed(); // rejected owner's destructor cannot release us.
 }
 restored();
 for(int cycle=0;cycle<100;++cycle){RadioPause pause;assert(pause.acquire(30)==Result::Acquired);pause.release();restored();}
 assert(feeds>0);
 asyncMode=true;const auto before=now;
 // Cancel while optional HTTPS is still live: retain admission, no worker takeover.
 services::firmware_update::noticeActive=true;
 requestPeripheralIo();assert(!peripheralIoReady());
 {PeriodicWork notice;assert(!notice.tryEnter());}
 resumeAfterPeripheralIo();services::firmware_update::noticeActive=false;restored();
 // An admitted HTTP owner may not have published its worker state yet.
 {PeriodicWork admitted;assert(admitted.tryEnter());requestPeripheralIo();
  assert(!peripheralIoReady());admitted.release();assert(peripheralIoReady());
  resumeAfterPeripheralIo();restored();}
 // A cache operation may drain before svc admission can close.
 failure=1;requestPeripheralIo();assert(!peripheralIoReady());
 {ServiceWork quoteDrain;assert(quoteDrain);}
 resumeAfterPeripheralIo();failure=0;restored();
 // Preserve an admitted svc iteration, then wait for all worker acknowledgments.
 auto inFlight=std::unique_ptr<ServiceWork>(new ServiceWork);
 requestPeripheralIo();assert(!peripheralIoReady());closed();inFlight.reset();
 workerLive=pollerLive=true;widgetAck=false;
 assert(!peripheralIoReady());assert(upload&&widget&&netStop);closed();
 assert(!suspendForPeripheralIo(30)); // Pending is not a usable nested reservation.
 workerLive=false;assert(!peripheralIoReady());
 pollerLive=false;assert(!peripheralIoReady());
 widgetAck=true;assert(peripheralIoReady());
 requestPeripheralIo();assert(peripheralIoReady());
 resumeAfterPeripheralIo();closed();assert(upload&&widget);
 resumeAfterPeripheralIo();restored();assert(now==before);
 // Cancel while the upload is still unwinding; resume requests never free its resources.
 workerLive=true;requestPeripheralIo();assert(!peripheralIoReady());
 resumeAfterPeripheralIo();workerLive=false;restored();assert(now==before);
}
''')
    subprocess.run(['c++', '-std=c++11', '-fsanitize=address,undefined', '-I'+str(root), '-I'+str(ROOT/'include'), str(ROOT/'src/services/background_network.cpp'), str(root/'main.cpp'), str(ROOT/'src/hal/audio_retirement.cpp'), '-o', str(root/'test')], check=True)
    subprocess.run([str(root/'test')], check=True)
print('PASS radio handoff: every consumer timeout, in-flight service work, exclusive ownership, rollback and repeated release under sanitizers')
