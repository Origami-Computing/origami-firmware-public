"""Verify upload worker lifetime using actual production method bodies.

Controlled fake scheduler; no SD/network/device operations. Default checks the
repair; --source-ref b03a5052 characterizes the original failures. Neither mode
is evidence of device crash frequency or FreeRTOS stack reclamation.
"""
from pathlib import Path
import argparse
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--source-ref', help='Characterize the old methods at a pinned git revision')
parser.add_argument('--thread-sanitizer', action='store_true')
args = parser.parse_args()
source = subprocess.check_output(['git','show',args.source_ref+':src/upload_queue.cpp'],cwd=ROOT,text=True) if args.source_ref else (ROOT / 'src/upload_queue.cpp').read_text()

def method(marker):
    start = source.index(marker)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
struct Task {bool alive=true;};
using TaskHandle_t=Task*;using TickType_t=uint32_t;
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_SPIRAM 2
constexpr int WL_CONNECTED=1;
struct {int status(){return WL_CONNECTED;}} WiFi;
struct {void println(const char*){}template<class... A>void printf(const char*,A...){}} Serial;
unsigned now=0,staleNotifications=0,releases=0;
Task task;TaskHandle_t current=nullptr;
std::function<void()> duringSpawn,beforeNotify;
void invoke(std::function<void()>& hook){if(hook){auto f=hook;hook=nullptr;f();}}
uint32_t millis(){return now;}
void vTaskDelay(uint32_t n){now+=n;}
unsigned ulTaskNotifyTake(int,uint32_t){return 0;}
unsigned uxTaskGetStackHighWaterMark(TaskHandle_t){return 1024;}
void vTaskDelete(TaskHandle_t h){assert(!h&&current);current->alive=false;current=nullptr;}
void xTaskNotifyGive(TaskHandle_t h){invoke(beforeNotify);if(!h||!h->alive)++staleNotifications;}
unsigned heap_caps_get_free_size(int){return 65536;}
unsigned heap_caps_get_largest_free_block(int){return 32768;}
namespace core {namespace watchdog {void subscribe(){}void feed(){}void unsubscribe(){}}}
namespace upload_queue_policy {bool deadlinePending(uint32_t now,uint32_t deadline){return deadline&&int32_t(now-deadline)<0;}}
namespace hal {namespace tasks {
enum class Id {UploadQ};
void recordCurrentStack(Id){} // Diagnostic-only HAL observation; no scheduling effect.
void deleteCurrent(Id){vTaskDelete(nullptr);}
TaskHandle_t spawn(Id,void(*)(void*),void*){task={};invoke(duringSpawn);return &task;}
}}
class UploadQueue {public:
 bool m_initialized=true,m_uploadPaused=false,m_workerStop=false,m_networkHeld=false;
 TaskHandle_t m_worker=nullptr;uint32_t m_nextWorkerStartAt=0;
 bool (*m_networkAcquire)(uint32_t)=nullptr;void (*m_networkRelease)()=nullptr;
 bool hasRunnableItem(){return true;}int pendingCount(){return 0;}
 bool processOnePending(){assert(false);return false;}
 static void workerTaskTrampoline(void*);
 void workerLoop();bool startWorker();bool stopWorkerForVoice(uint32_t);
};
'''
if not args.source_ref:
    code = code.replace('#include <cassert>', '#include <cassert>\n#include <atomic>\n#include <thread>\n#include <mutex>\n#include <chrono>')
    code = code.replace('unsigned now=0,staleNotifications=0,releases=0;', 'std::atomic<unsigned> now{0}; unsigned staleNotifications=0,releases=0; bool spawnFails=false; std::atomic<bool> publicationWait{false};')
    code = code.replace('bool alive=true;', 'std::atomic<bool> alive{true};')
    code = code.replace('task={};','task.alive=true;')
    code = code.replace('invoke(duringSpawn);return &task;', 'invoke(duringSpawn);return spawnFails?nullptr:&task;')
    code = code.replace('void vTaskDelay(uint32_t n){now+=n;}', 'void vTaskDelay(uint32_t n){now+=n;publicationWait=true;std::this_thread::sleep_for(std::chrono::milliseconds(1));}')
    code = code.replace('class UploadQueue {public:', '''using portMUX_TYPE=std::mutex;
#define portENTER_CRITICAL(p) (p)->lock()
#define portEXIT_CRITICAL(p) (p)->unlock()
class UploadQueue {public:
 enum class WorkerPhase {Idle,Starting,Running,Retiring};
 mutable portMUX_TYPE m_workerMux; WorkerPhase m_workerPhase=WorkerPhase::Idle;
 unsigned m_notifications=0;
 bool workerRunning() const;void notifyWorker();void retireWorker();void requestStopForVoice();''')
    code = code.replace('bool m_initialized=true,m_uploadPaused=false,m_workerStop=false,m_networkHeld=false;', 'bool m_initialized=true,m_networkHeld=false; std::atomic<bool> m_uploadPaused{false},m_workerStop{false};')
    code += '\n'.join(method(marker) for marker in ('bool UploadQueue::workerRunning(', 'void UploadQueue::notifyWorker(', 'void UploadQueue::retireWorker(', 'void UploadQueue::requestStopForVoice('))
code += '\n'.join(method(marker) for marker in (
    'void UploadQueue::workerTaskTrampoline(',
    'void UploadQueue::workerLoop(',
    'bool UploadQueue::startWorker(',
    'bool UploadQueue::stopWorkerForVoice(',
))
code += r'''
void reset(){task={};now=staleNotifications=releases=0;current=nullptr;duringSpawn=beforeNotify=nullptr;}
void run(UploadQueue& q){current=&task;q.workerLoop();}
void lease(UploadQueue& q){q.m_networkAcquire=[](uint32_t){return true;};q.m_networkRelease=[](){++releases;};}
int main(){
 reset();UploadQueue first;lease(first);
 duringSpawn=[&]{run(first);};
 bool started=first.startWorker();
 assert(started&&first.m_worker&&!task.alive&&staleNotifications==1&&releases==1);
 puts("REPRODUCED: worker exits before spawn returns; creator publishes and notifies deleted handle");

 reset();UploadQueue second;lease(second);bool stopped=false;
 duringSpawn=[&]{stopped=second.stopWorkerForVoice(0);};
 assert(second.startWorker());
 assert(stopped&&second.m_worker&&task.alive&&second.m_networkHeld&&releases==0);
 puts("REPRODUCED: stop reports success during creation while worker/network lease remain active");
 run(second);assert(releases==1);

 reset();UploadQueue third;lease(third);assert(third.startWorker());
 beforeNotify=[&]{run(third);};
 assert(third.stopWorkerForVoice(10));
 assert(staleNotifications==1&&!task.alive&&releases==1);
 puts("REPRODUCED: stop copies handle, worker exits, then notification uses deleted handle");
}
'''
if not args.source_ref:
    code = code[:code.index('void reset(){')] + r'''
void reset(){task.alive=true;now=0;staleNotifications=releases=0;current=nullptr;duringSpawn=beforeNotify=nullptr;spawnFails=false;publicationWait=false;}
void run(UploadQueue& q){current=&task;q.workerLoop();}
void lease(UploadQueue& q){q.m_networkAcquire=[](uint32_t){return true;};q.m_networkRelease=[](){++releases;};}
int main(){
 reset();UploadQueue first;lease(first);std::thread worker;
 duringSpawn=[&]{worker=std::thread([&]{run(first);});while(!publicationWait)std::this_thread::yield();assert(task.alive&&first.workerRunning());};
 assert(first.startWorker());worker.join();
 assert(!first.workerRunning()&&!task.alive&&!staleNotifications&&releases==1);
 puts("PASS early worker waits for handle publication");

 reset();UploadQueue second;lease(second);bool stopped=true;
 duringSpawn=[&]{stopped=second.stopWorkerForVoice(0);};
 assert(second.startWorker());assert(!stopped&&second.workerRunning()&&!second.m_uploadPaused);
 run(second);assert(releases==1);
 puts("PASS stop during creation times out honestly and preserves resume policy");

 reset();UploadQueue third;lease(third);assert(third.startWorker());
 beforeNotify=[&]{worker=std::thread([&]{run(third);});for(;;){std::lock_guard<std::mutex> lock(third.m_workerMux);if(third.m_workerPhase==UploadQueue::WorkerPhase::Retiring)break;}assert(task.alive);};
 assert(third.stopWorkerForVoice(1000));worker.join();
 assert(!staleNotifications&&!task.alive&&releases==1);
 puts("PASS retirement waits for outstanding notification before deleting task");

 reset();UploadQueue fourth;lease(fourth);spawnFails=true;
 assert(!fourth.startWorker()&&!fourth.workerRunning()&&releases==1);
 spawnFails=false;now=6000;assert(fourth.startWorker());run(fourth);assert(releases==2);
 puts("PASS spawn failure releases once and permits later retry");

 reset();UploadQueue fifth;lease(fifth);
 fifth.m_networkAcquire=[](uint32_t){return false;};
 assert(!fifth.startWorker()&&!fifth.workerRunning()&&releases==0);
 assert(fifth.stopWorkerForVoice(0)&&fifth.m_uploadPaused);
 assert(!fifth.startWorker());
 puts("PASS rejected network admission and idle pause leave no worker");

 reset();UploadQueue sixth;lease(sixth);
 duringSpawn=nullptr;
 // Check admission before task creation, while network acquisition still owns it.
 static UploadQueue* admitting=nullptr;admitting=&sixth;
 sixth.m_networkAcquire=[](uint32_t){assert(admitting->workerRunning());assert(!admitting->stopWorkerForVoice(0));return true;};
 assert(sixth.startWorker());run(sixth);assert(releases==1);
 puts("PASS network acquisition is visible to stop before a task exists");
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path / 'probe.cpp').write_text(code)
    subprocess.run(['c++', '-std=c++14', '-pthread', '-fsanitize='+('thread' if args.thread_sanitizer else 'address,undefined'),
                    str(path / 'probe.cpp'), '-o', str(path / 'probe')], check=True)
    subprocess.run([str(path / 'probe')], check=True, timeout=30)
