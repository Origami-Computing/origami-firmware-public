"""Audit actual Morning quote service handoff using offline outcome fakes.

The default requires the desired behavior; --expect-bug characterizes the
pre-repair ordering bug. No device, network, persistent cache or SD is touched.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--expect-bug',action='store_true')
p.add_argument('--thread-sanitizer',action='store_true')
p.add_argument('--cache-reset-audit',action='store_true',help='Check development cache reset versus pending admission')
p.add_argument('--negative-control',action='store_true',help='Use hash-verified frozen build161 source; expect the bug')
p.add_argument('--source',type=Path,default=ROOT/'src/services/morning_quote.cpp')
a=p.parse_args()
if a.negative_control:
    a.expect_bug=True
    a.source=ROOT/'test/fixtures/morning_quote_handoff/morning_quote.cpp'
    meta=json.loads(a.source.with_name('manifest.json').read_text())
    assert hashlib.sha256(a.source.read_bytes()).hexdigest()==meta['sha256']
s=a.source.read_text()
def fn(signature):
    start=s.index(signature+' {');end=s.index('{',start)+1;depth=1
    while depth:
        depth+=(s[end]=='{')-(s[end]=='}');end+=1
    return s[start:end]
state=s[s.index('portMUX_TYPE s_mux'):s.index('#ifdef ORIGAMI_DEV_SERIAL_INPUT',s.index('portMUX_TYPE s_mux'))]
code=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <thread>
#include <functional>
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(m) (m)->lock()
std::function<void()> onUnlock;
void leave(std::mutex* m){m->unlock();if(onUnlock){auto hook=onUnlock;onUnlock=nullptr;hook();}}
#define portEXIT_CRITICAL(m) leave(m)
#define pdMS_TO_TICKS(n) (n)
void vTaskDelay(unsigned){std::this_thread::yield();}
uint32_t millis(){return 0;}
size_t strlcpy(char* d,const char* s,size_t n){size_t l=strlen(s);if(n){size_t c=l<n-1?l:n-1;memcpy(d,s,c);d[c]=0;}return l;}
struct Logger{void println(const char*){}template<class...T>void printf(const char*,T...){}} Serial;
enum class Status:uint8_t{Idle,WaitingForNetwork,Fetching,Ready,Error};
struct Record{unsigned day;};
constexpr char kNvsNamespace[]="morning_quote";
constexpr uint32_t kResourceRetryMs=30000,kAutomaticRetryMs=900000;
std::function<void()> onConnected,onIdentity,onSubmit,onStore,onHistory;
bool identityOk=true,submitOk=true,recordOk=true,storeOk=true,autoReply=false,online=true;
void call(std::function<void()>& callback){if(callback){auto hook=callback;callback=nullptr;hook();}}
namespace hal{namespace wifi{bool connected(){const bool value=online;call(onConnected);return value;}}}
namespace services{namespace agent_coordination{bool paused=false;bool isSessionActive(){return false;}bool suspendRequested(){return paused;}}}
namespace core{
namespace store{bool putU32(const char*,const char*,uint32_t){call(onIdentity);return identityOk;}}
namespace device_agent_text{
enum class Outcome{Ready,Unsupported,Error};
struct Result{Outcome outcome=Outcome::Ready;char text[200]="A controlled quote for handoff tests.";char error[64]="test failure";bool retryable=false;};
Outcome nextOutcome=Outcome::Ready;bool available=false;unsigned submitted=0;char lastId[81]={};
void cancel(){}bool take(Result& out){bool held=available || autoReply;available=false;if(held)out.outcome=nextOutcome;return held;}
bool submit(const char* id,const char*){++submitted;strlcpy(lastId,id,sizeof(lastId));call(onSubmit);return submitOk;}
}}
bool formatLocalDate(time_t epoch,uint32_t* day,char date[11]){*day=uint32_t(epoch);strlcpy(date,"2026-09-26",11);return epoch>0;}
bool copyRecordForDay(uint32_t,Record*){return false;}
bool fillRecord(const char*,uint32_t day,Record* r){r->day=day;return recordOk;}
bool storeRecord(const Record&){call(onStore);return storeOk;}
bool appendHistory(const Record&,const char*){call(onHistory);return true;}
void resumeAfterPeripheralIo();
'''+state+'\n'+(fn('void setStatusLocked(Status value, const char* text)')+'\n' if 'void setStatusLocked(' in s else '')+(fn('void deferAttempt(uint32_t revision, uint32_t retryAt, const char* error)')+'\n' if 'void deferAttempt(' in s else '')+'\n'.join(fn(x) for x in [
 'void setStatus(Status value, const char* text)',
 'void requestForEpoch(time_t epochSeconds, bool force)',
 'void serviceBackground(uint32_t nowMs)',
 *(['bool pollPeripheralPause()'] if 'bool pollPeripheralPause()' in s else []),
 'bool suspendForPeripheralIo(uint32_t timeoutMs)',
 'void resumeAfterPeripheralIo()',
 'bool missCacheForDevelopment()',
 'Status status()',
 'const char* statusText()',
 'bool fetching()'])+r'''
int main(){
#ifdef CACHE_RESET_PROBE
 requestForEpoch(20260926,true);
 onConnected=[] {assert(missCacheForDevelopment());};
 serviceBackground(1);
 assert(s_targetDay==0);
#ifdef EXPECT_BUG
 assert(core::device_agent_text::submitted==1);
 puts("REPRODUCED: cache reset leaves an old admission snapshot usable");
#else
 assert(core::device_agent_text::submitted==0 && !s_taskRunning);
 puts("PASS cache reset invalidates pending admission");
#endif
#elif defined(THREAD_PROBE)
 std::atomic<unsigned> ready{0};std::atomic<bool> go{false};
 auto gate=[&]{++ready;while(!go.load())std::this_thread::yield();};
 std::thread writer([&]{gate();for(unsigned i=0;i<100000;++i)setStatus((i&1)?Status::Fetching:Status::Idle,"fixture");});
 std::thread reader([&]{gate();for(unsigned i=0;i<100000;++i){(void)status();(void)fetching();}});
 while(ready.load()!=2)std::this_thread::yield();go=true;writer.join();reader.join();
#ifndef EXPECT_BUG
 ready=0;go=false;autoReply=true;
 std::thread requests([&]{gate();for(unsigned i=0;i<10000;++i)requestForEpoch(20260926+(i&1),i%3==0);});
 std::thread worker([&]{gate();for(unsigned i=0;i<10000;++i)serviceBackground(i);});
 std::thread observer([&]{gate();for(unsigned i=0;i<10000;++i){(void)status();(void)fetching();}});
 std::thread pause([&]{gate();for(unsigned i=0;i<10000;++i){(void)suspendForPeripheralIo(0);resumeAfterPeripheralIo();}});
 while(ready.load()!=4)std::this_thread::yield();go=true;
 requests.join();worker.join();observer.join();pause.join();
 puts("PASS TSan concurrent requests, worker, observations and suspension");
#endif

#else
 // A worker can run between request publication and the caller's status update.
 onUnlock=[] {serviceBackground(1);};
 requestForEpoch(20260926,true);
 assert(core::device_agent_text::submitted==1 && s_taskRunning);
#ifdef EXPECT_BUG
 assert(status()==Status::Idle && !fetching());
 puts("REPRODUCED: request's late Queued publication hides active Fetching");
#else
 assert(status()==Status::Fetching && fetching());
 puts("PASS request publication cannot hide active Fetching");
#endif
 // The same late publication can also overwrite a successfully completed turn.
 s_taskRunning=false;core::device_agent_text::submitted=0;
 onUnlock=[] {serviceBackground(2);core::device_agent_text::available=true;serviceBackground(3);};
 requestForEpoch(20260927,true);
 assert(core::device_agent_text::submitted==1 && !s_taskRunning);
#ifdef EXPECT_BUG
 assert(status()==Status::Idle);
 puts("REPRODUCED: request's late Queued publication hides completed Ready");
#else
 assert(status()==Status::Ready);
 puts("PASS request publication cannot hide completed Ready");
 auto fresh=[] {
   s_taskRunning=false;s_targetDay=0;s_force=false;s_newGeneration=false;s_nextAttemptMs=0;
   s_peripheralPaused=false;s_status=Status::Idle;
   identityOk=submitOk=recordOk=storeOk=online=true;autoReply=false;
   core::device_agent_text::available=false;core::device_agent_text::submitted=0;
   core::device_agent_text::nextOutcome=core::device_agent_text::Outcome::Ready;services::agent_coordination::paused=false;
 };
 // A transfer pause survives quote retry deadlines, without losing the request.
 fresh();requestForEpoch(20260926,true);serviceBackground(1);
 core::device_agent_text::nextOutcome=core::device_agent_text::Outcome::Unsupported;
 core::device_agent_text::available=true;serviceBackground(2);
 assert(!s_taskRunning && s_nextAttemptMs==30002);
 services::agent_coordination::paused=true;
 const auto generation=s_requestGeneration;
 for(unsigned now=30002;now<300000;now+=1000)serviceBackground(now);
 assert(core::device_agent_text::submitted==1 && !s_taskRunning && s_requestGeneration==generation);
 services::agent_coordination::paused=false;serviceBackground(300000);
 assert(core::device_agent_text::submitted==2 && s_taskRunning && s_requestGeneration==generation);
 // Already-finished replies still drain while another operation holds a pause.
 core::device_agent_text::nextOutcome=core::device_agent_text::Outcome::Ready;
 core::device_agent_text::available=true;services::agent_coordination::paused=true;
 serviceBackground(300001);assert(!s_taskRunning && status()==Status::Ready);
 puts("PASS transport pause prevents recurring quote retries and release resumes the same request; terminal replies still drain");
 // A replacement during outside-lock readiness checks prevents stale admission.
 fresh();requestForEpoch(20260926,true);
 onConnected=[] {requestForEpoch(20260927,true);};
 serviceBackground(10);assert(core::device_agent_text::submitted==0);
 serviceBackground(11);assert(strstr(core::device_agent_text::lastId,"mq2:20260927:")!=nullptr);
 // An old offline observation cannot replace a newer online request's status.
 fresh();requestForEpoch(20260926,true);online=false;
 onConnected=[] {online=true;requestForEpoch(20260927,true);};serviceBackground(10);
 assert(status()==Status::Idle && s_nextAttemptMs==0);
 // Failed identity persistence must retain force and retry, but not delay newer work.
 fresh();requestForEpoch(20260926,true);identityOk=false;
 serviceBackground(100);assert(!s_taskRunning && s_newGeneration && s_nextAttemptMs==30100 && status()==Status::Error);
 requestForEpoch(20260926,false);assert(s_nextAttemptMs==30100 && status()==Status::Error);
 serviceBackground(30099);assert(core::device_agent_text::submitted==0);
 identityOk=true;serviceBackground(30100);assert(core::device_agent_text::submitted==1);
 fresh();requestForEpoch(20260926,true);identityOk=false;
 onIdentity=[] {requestForEpoch(20260927,true);};
 serviceBackground(100);assert(s_nextAttemptMs==0 && status()==Status::Idle && s_newGeneration);
 identityOk=true;serviceBackground(101);assert(strstr(core::device_agent_text::lastId,"mq2:20260927:")!=nullptr);
 // Submit refusal has bounded retry; newer requests survive that refusal.
 fresh();requestForEpoch(20260926,true);submitOk=false;serviceBackground(100);
 assert(!s_taskRunning && s_nextAttemptMs==30100);
 fresh();requestForEpoch(20260926,true);submitOk=false;
 onSubmit=[] {requestForEpoch(20260927,true);};serviceBackground(100);
 assert(s_nextAttemptMs==0 && status()==Status::Idle && s_newGeneration);
 // Manual retry while an old result is being stored remains a distinct next turn.
 fresh();requestForEpoch(20260926,true);serviceBackground(100);
 core::device_agent_text::available=true;
 onStore=[] {requestForEpoch(20260926,true);assert(fetching());};
 serviceBackground(101);assert(!s_taskRunning && s_newGeneration && s_nextAttemptMs==0 && status()==Status::Idle);
 serviceBackground(102);assert(core::device_agent_text::submitted==2);
 // Persistence stays owned through history; a pause may time out, not report quiescence.
 core::device_agent_text::available=true;
 onHistory=[] {assert(!suspendForPeripheralIo(0));};serviceBackground(103);
 assert(suspendForPeripheralIo(0));serviceBackground(104);resumeAfterPeripheralIo();
 // Failed storage uses 30 seconds; invalid completed content uses 15 minutes/new identity.
 fresh();requestForEpoch(20260926,true);serviceBackground(100);storeOk=false;
 core::device_agent_text::available=true;serviceBackground(101);assert(s_nextAttemptMs==30101 && !s_newGeneration);
 fresh();requestForEpoch(20260926,true);serviceBackground(100);recordOk=false;
 core::device_agent_text::available=true;serviceBackground(101);assert(s_nextAttemptMs==900101 && s_newGeneration);
 // Backoff comparison remains wrap-safe.
 fresh();requestForEpoch(20260926,true);serviceBackground(100);storeOk=false;
 core::device_agent_text::available=true;serviceBackground(UINT32_MAX-1000);
 const uint32_t deadline=s_nextAttemptMs;assert(deadline==28999);unsigned count=core::device_agent_text::submitted;
 serviceBackground(deadline-1);assert(core::device_agent_text::submitted==count);
 submitOk=true;serviceBackground(deadline);assert(core::device_agent_text::submitted==count+1);
 puts("PASS replacement, identity/submit failure, persistence ownership, completion and retry/wrap");

#endif
#endif
}
'''
with tempfile.TemporaryDirectory(prefix='kami-morning-handoff-') as tmp:
    d=Path(tmp);(d/'probe.cpp').write_text(code)
    flags=['-fsanitize=thread','-DTHREAD_PROBE'] if a.thread_sanitizer else ['-fsanitize=address,undefined']
    if a.expect_bug:flags+=['-DEXPECT_BUG']
    if a.cache_reset_audit:flags+=['-DCACHE_RESET_PROBE']
    subprocess.run(['c++','-std=c++11','-g','-O1','-pthread',*flags,str(d/'probe.cpp'),'-o',str(d/'probe')],check=True)
    result=subprocess.run([str(d/'probe')],text=True,capture_output=True)
    if a.expect_bug and a.thread_sanitizer:
        assert result.returncode and 'ThreadSanitizer: data race' in result.stderr and 's_status' in result.stderr,result.stderr
        print('REPRODUCED: actual status writer/readers race on s_status')
    else:
        print(result.stdout,end='')
        if result.returncode:print(result.stderr)
        assert result.returncode==0
