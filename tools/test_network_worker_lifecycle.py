"""Compile the production lifecycle, offline worker and HAL against a threaded scheduler.
Controlled gates force publication, cancellation, notification and deletion races.
Does not emulate HTTP, FreeRTOS priorities or idle-task memory reclamation.
"""
from pathlib import Path
import argparse,re,subprocess,tempfile,hashlib,json
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--thread-sanitizer',action='store_true')
p.add_argument('--restart-admission-probe',action='store_true',help='Compatibility alias: admission is now included by default')
p.add_argument('--negative-control',action='store_true',help='Reproduce stale admission using frozen build156 coordinator bodies')
args=p.parse_args()
source=(ROOT/'src/services/net_poll.cpp').read_text()
def function(signature):
 a=source.index(signature+' {');b=source.index('{',a)+1;depth=1
 while depth:depth+=(source[b]=='{')-(source[b]=='}');b+=1
 return source[a:b]
with tempfile.TemporaryDirectory() as directory:
 temp=Path(directory)
 def write(name,data):
  f=temp/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text(data)
 write('freertos/FreeRTOS.h', '''#pragma once
#include <stdint.h>
using BaseType_t=int;using UBaseType_t=unsigned;
using TaskFunction_t=void(*)(void*);
struct FakeTask;using TaskHandle_t=FakeTask*;
#define pdPASS 1
#define pdFAIL 0
using StackType_t=uint8_t;struct StaticTask_t{int unused;};
#define pdTRUE 1
#define pdMS_TO_TICKS(x) (x)
''')
 write('freertos/task.h', '''#pragma once
#include "freertos/FreeRTOS.h"
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t,const char*,uint32_t,void*,UBaseType_t,TaskHandle_t*,BaseType_t);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t);
void vTaskDelete(TaskHandle_t);
TaskHandle_t xTaskGetCurrentTaskHandle();
TaskHandle_t xTaskCreateStaticPinnedToCore(TaskFunction_t,const char*,uint32_t,void*,UBaseType_t,StackType_t*,StaticTask_t*,BaseType_t);
void vTaskDelay(uint32_t);
void xTaskNotifyGive(TaskHandle_t);
unsigned ulTaskNotifyTake(BaseType_t,uint32_t);
''')
 write('esp_freertos_hooks.h', '''#pragma once
#define ESP_OK 0
int esp_register_freertos_idle_hook_for_cpu(bool(*)(),unsigned);
''')
 write('Arduino.h', '''#pragma once
#include <stdint.h>
struct Logger {void println(const char*){}template<class... A>void printf(const char*,A...){}};extern Logger Serial;
uint32_t millis();
''')
 write('esp_heap_caps.h', '''#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1
inline size_t heap_caps_get_free_size(int){return 65536;}
inline size_t heap_caps_get_largest_free_block(int){return 32768;}
''')
 write('freertos/portmacro.h', r'''#pragma once
#include <mutex>
#include <cassert>
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
extern thread_local unsigned criticalDepth;
inline void portENTER_CRITICAL(portMUX_TYPE* m){m->lock();++criticalDepth;}
inline void portEXIT_CRITICAL(portMUX_TYPE* m){--criticalDepth;m->unlock();}
''')
 write('core/telemetry.h', '#pragma once\n#define LOGI(...) ((void)0)\n')
 code=Path(__file__).with_name('network_worker_lifecycle_test.cpp').read_text().replace('namespace background_network {','namespace background_network { std::atomic<bool> voicePriority{false};')
 background=(ROOT/'src/services/background_network.cpp').read_text()
 if args.negative_control:
  frozen=ROOT/'test/fixtures/background_restart_admission'
  background=(frozen/'background.cpp').read_text()
  assert hashlib.sha256(background.encode()).hexdigest()==json.loads((frozen/'manifest.json').read_text())['sha256']
 def background_body(signature):
  a=background.index(signature+' {');b=background.index('{',a)+1;depth=1
  while depth:depth+=(background[b]=='{')-(background[b]=='}');b+=1
  return background[a:b]
 bridge=r'''
namespace hal {namespace audio {bool retirementPending(){return false;}}}
namespace services {namespace background_network {
struct AudioSuspension {enum class Poller {NotRequested,Stopped,StopPending};bool uploadsReady;Poller poller;};
std::atomic<bool> voicePriority{false};
bool audioPriorityActive(){return voicePriority.load();}
std::function<void()> beforeStart;
bool queueReady=true, uploadRunning=false;
struct Queue {
bool workerRunning(){if(beforeStart){auto f=beforeStart;beforeStart=nullptr;f();}return uploadRunning;}
bool resumeWorker(){return true;}bool stopWorkerForVoice(uint32_t){return true;}
} queue;
namespace uploads {Queue* readyQueue(){return queueReady?&queue:nullptr;}}
'''
 if args.negative_control:
  bridge+=re.search(r'^std::atomic<[^>]+> s_restartNetPoll[^\n]+',background,re.M)[0]+'\n'
  bridge+=background_body('void setPollerRestart(bool requested)')+'\n'
 for sig in ['void tick()','void resumeAfterMusicAudio()','AudioSuspension suspendForMusicAudio(uint32_t timeoutMs)']:
  bridge+=background_body(sig)+'\n'
 bridge+='}}\n'
 bridge+='#define NEGATIVE_CONTROL '+str(int(args.negative_control))+'\n'
 bridge+=Path(__file__).with_name('background_restart_lifecycle_test.cpp').read_text()
 code=code.replace('int main(){',bridge+'int main(){')
 code=code.replace('assert(queries==0);','assert(queries==0);if(testRestartAdmission())return 0;')
 # This harness deliberately keeps Wi-Fi offline. Network admission must never run;
 # production ownership/contention is covered by test_background_network.py.
 code+='\nnamespace services {namespace background_network {struct PeriodicWork {bool tryEnter(){assert(false);return false;}bool yieldIfNeeded(){assert(false);return false;}void release(){assert(false);}};}}\n'
 code+='namespace services {namespace net_poll {void phase(const char*){} void publishRegistrationPriority(bool){} bool onboardingRequestPending(){assert(false);return false;}}}\n'
 code+='#include "registration_retry.h"\nnamespace services {namespace net_poll {RegistrationRetry s_registrationRetry;}}\n'
 code+='namespace services {namespace account_info {void poll(){assert(false);}}}\n'
 code+= '\nnamespace services {namespace net_poll {\n'
 code+=re.search(r'^constexpr uint32_t kTickMs[^\n]+',source,re.M)[0]+'\n'
 code+=function('void lifecycle::run(void*)')+'\n}}\n'
 write('test.cpp',code)
 subprocess.run(['c++','-std=c++14','-pthread','-g',
  '-fsanitize='+('thread' if args.thread_sanitizer else 'address,undefined'),
  '-I'+str(temp),'-I'+str(ROOT/'include'),'-I'+str(ROOT/'src/services'),
  str(temp/'test.cpp'),str(ROOT/'src/services/net_poll_lifecycle.cpp'),
  str(ROOT/'src/hal/hal_tasks.cpp'),'-o',str(temp/'test')],check=True)
 subprocess.run([str(temp/'test')],check=True,timeout=45)
