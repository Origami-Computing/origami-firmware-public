// Offline RTOS boundary harness; compiled by test_network_worker_lifecycle.py.
#include "hal/hal_tasks.h"
#include "services/net_poll.h"
#include "net_poll_lifecycle.h"
#include "Arduino.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <cstdio>
using namespace std::chrono;
using namespace services::net_poll;
Logger Serial;
thread_local unsigned criticalDepth=0;
struct FakeTask {
 std::thread thread;
 std::atomic<bool> alive{true};
 std::mutex mutex;
 std::condition_variable cv;
 bool notified=false;
};
thread_local FakeTask* currentTask=nullptr;
TaskHandle_t xTaskGetCurrentTaskHandle(){return currentTask;}
bool (*idleHook)()=nullptr;
int esp_register_freertos_idle_hook_for_cpu(bool(*hook)(),unsigned core){assert(core==0);idleHook=hook;return 0;}
std::vector<std::unique_ptr<FakeTask>> tasks;
std::mutex hooksMutex;
std::function<void()> duringCreate,duringSubscribe,duringUnsubscribe,duringNotify,beforeDelete;
std::atomic<bool> allocationFails{false};
std::atomic<unsigned> subscribed{0},unsubscribed{0},queries{0};
std::atomic<uint32_t> clockOffset{0};
const auto origin=steady_clock::now();
void invoke(std::function<void()>& hook){
 std::function<void()> action;
 {std::lock_guard<std::mutex> lock(hooksMutex);action=std::move(hook);hook=nullptr;}
 if(action)action();
}
struct Gate {
 std::mutex mutex;std::condition_variable cv;bool entered=false,released=false;
 void block(){std::unique_lock<std::mutex> lock(mutex);entered=true;cv.notify_all();cv.wait(lock,[&]{return released;});}
 void wait(){std::unique_lock<std::mutex> lock(mutex);assert(cv.wait_for(lock,seconds(3),[&]{return entered;}));}
 void release(){std::lock_guard<std::mutex> lock(mutex);released=true;cv.notify_all();}
 void install(std::function<void()>& hook){std::lock_guard<std::mutex> lock(hooksMutex);hook=[this]{block();};}
};
template<class F>void eventually(F done){auto deadline=steady_clock::now()+seconds(3);while(!done()){assert(steady_clock::now()<deadline);std::this_thread::sleep_for(milliseconds(1));}}
uint32_t millis(){return uint32_t(duration_cast<milliseconds>(steady_clock::now()-origin).count())+clockOffset.load();}
void vTaskDelay(uint32_t ms){assert(!criticalDepth);std::this_thread::sleep_for(milliseconds(ms));}
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t entry,const char*,uint32_t,void* arg,UBaseType_t,TaskHandle_t* output,BaseType_t){
 assert(!criticalDepth);if(allocationFails){invoke(duringCreate);return 0;}
 std::unique_ptr<FakeTask> task(new FakeTask);auto raw=task.get();*output=raw;
 tasks.push_back(std::move(task));
 raw->thread=std::thread([=]{currentTask=raw;entry(arg);assert(!raw->alive);if(idleHook){idleHook();idleHook();}});
 invoke(duringCreate);return pdPASS;
}
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t fn,const char* name,uint32_t stack,void* arg,UBaseType_t priority,StackType_t*,StaticTask_t*,BaseType_t core){
 TaskHandle_t handle=nullptr;xTaskCreatePinnedToCore(fn,name,stack,arg,priority,&handle,core);return handle;
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task){assert(!criticalDepth);if(!task)task=currentTask;assert(task&&task->alive);++queries;return 512;}
void vTaskDelete(TaskHandle_t task){assert(!criticalDepth&&!task&&currentTask);invoke(beforeDelete);currentTask->alive=false;}
void xTaskNotifyGive(TaskHandle_t task){
 assert(!criticalDepth&&task&&task->alive);
 {std::lock_guard<std::mutex> lock(task->mutex);task->notified=true;task->cv.notify_all();}
 invoke(duringNotify);assert(task->alive);
}
unsigned ulTaskNotifyTake(BaseType_t,uint32_t timeout){
 assert(!criticalDepth&&currentTask);
 std::unique_lock<std::mutex> lock(currentTask->mutex);
 currentTask->cv.wait_for(lock,milliseconds(timeout),[]{return currentTask->notified;});
 bool notified=currentTask->notified;currentTask->notified=false;return notified;
}
namespace core {namespace watchdog {
 void subscribe(){++subscribed;invoke(duringSubscribe);}void feed(){}
 void unsubscribe(){++unsubscribed;invoke(duringUnsubscribe);}
}}
namespace hal {namespace wifi {bool connected(){return false;}}}
namespace services {
 namespace eyes_asset {void service(){assert(false);}}
 namespace automation {void pollTransportNow(){assert(false);}}
 namespace device_fs {void poll(){assert(false);}}
}
bool ensureRegistered(){assert(false);return false;}
void sendLatestHealth(){assert(false);}void drainTelemetry(){assert(false);}
bool due(uint32_t,uint32_t*,uint32_t){assert(false);return false;}
constexpr uint32_t AUTOMATION_POLL_INTERVAL_MS=1000,TELEMETRY_DRAIN_INTERVAL_MS=1000,DEVICE_FS_POLL_INTERVAL_MS=1000;
void joinAll(){for(auto& task:tasks)if(task->thread.joinable())task->thread.join();}
void finish(){assert(stop(1000));joinAll();assert(!running());}
int main(){
 assert(!running()&&stop(0));allocationFails=true;assert(!start()&&!running());allocationFails=false;
 hal::tasks::dumpStackWatermarks();assert(queries==0);
 // A worker can run immediately but cannot subscribe before publication.
 {
 Gate gate;gate.install(duringCreate);unsigned before=subscribed;auto count=tasks.size();
 bool created=false;std::thread creator([&]{created=start();});gate.wait();
 assert(running()&&!start()&&!stop(0));assert(tasks.size()==count+1&&subscribed==before);
 gate.release();creator.join();assert(created);eventually([]{return !running();});joinAll();
 }
 puts("PASS creation publication, duplicate-start rejection, stop during creation, allocation failure");
 // Stop remains cancellable until the worker claims irreversible retirement.
 {
 Gate gate;gate.install(duringSubscribe);assert(start());gate.wait();auto count=tasks.size();
 clockOffset=UINT32_MAX-millis()-5;auto began=steady_clock::now();
 assert(!stop(25)&&running()&&stopRequested());
 assert(!cancelHandshakeForCurrentTask()); // Caller cannot cancel another task.
 currentTask=tasks.back().get(); assert(cancelHandshakeForCurrentTask()); currentTask=nullptr;
 // millis() truncates to whole milliseconds, so its 25-tick deadline can
 // arrive up to one millisecond before 25 ms of steady-clock duration.
 assert(steady_clock::now()-began>=milliseconds(24));clockOffset=0;
 assert(start()&&!stopRequested()&&tasks.size()==count);
 currentTask=tasks.back().get(); assert(!cancelHandshakeForCurrentTask()); currentTask=nullptr;gate.release();
 assert(running());finish();
 }
 puts("PASS stop timeout, clock wrap, resume before retirement without duplicate task");
 {
 Gate gate;gate.install(duringUnsubscribe);assert(start());stop(0);gate.wait();
 assert(running()&&!start());gate.release();eventually([]{return !running();});joinAll();
 assert(start());finish();
 }
 puts("PASS irreversible retirement rejects resume; retry creates a worker");
 // Notification holds the handle valid even if its wakeup causes retirement.
 {
 Gate gate;gate.install(duringNotify);assert(start());auto task=tasks.back().get();auto before=unsubscribed.load();
 bool stopped=false;std::thread stopper([&]{stopped=stop(1000);});gate.wait();
 eventually([&]{return unsubscribed>before;});
 assert(running()&&task->alive&&!start());gate.release();stopper.join();assert(stopped);joinAll();
 }
 puts("PASS notification borrow prevents self-deletion during RTOS notification");
 // Quiescence is not deletion/reclamation. A retired worker never clears its successor.
 {
 Gate gate;gate.install(beforeDelete);assert(start());stop(0);gate.wait();
 auto retired=tasks.back().get();assert(!running()&&stop(0)&&retired->alive);
 auto before=queries.load();hal::tasks::dumpStackWatermarks();assert(queries==before);
 assert(!start()); // Shared stack cannot be reused before idle cleanup.
 gate.release();retired->thread.join();assert(start());auto successor=tasks.back().get();assert(successor!=retired);
 assert(running()&&successor->alive);finish();
 before=queries;hal::tasks::dumpStackWatermarks();assert(queries==before);
 }
 puts("PASS quiescence contract, successor isolation, diagnostics avoid retired task handles");
 for(int i=0;i<100;++i){assert(start());assert(start());finish();}
 assert(subscribed==unsubscribed&&queries==subscribed);
 puts("PASS 100 threaded restart cycles; each worker records its own stack exactly once");
}
