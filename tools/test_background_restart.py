"""Restart regression entry point.

Default uses the combined production lifecycle/coordinator threaded test under
TSan. --negative-control retains frozen build154 lost-intent/data-race evidence.
The build156 stale-admission control lives in test_network_worker_lifecycle.py.
"""
from pathlib import Path
import argparse,hashlib,json,re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--negative-control',action='store_true')
a=p.parse_args()
if not a.negative_control:
 subprocess.run(["python3",str(ROOT/"tools/test_network_worker_lifecycle.py"),"--thread-sanitizer"],check=True)
 raise SystemExit(0)
s=(ROOT/'src/services/background_network.cpp').read_text()
if a.negative_control:
 r=ROOT/'test/fixtures/background_restart';data=(r/'restart.cpp').read_bytes()
 assert hashlib.sha256(data).hexdigest()==json.loads((r/'manifest.json').read_text())['excerpt_sha256']
 s=data.decode()
def function(sig):
 start=s.index(sig+' {');end=s.index('{',start)+1;depth=1
 while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]
code=r'''
#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <thread>
struct Logger {void println(const char*){}} Serial;
struct AudioSuspension {enum class Poller {NotRequested,Stopped,StopPending};bool uploadsReady;Poller poller;};
namespace hal {namespace audio {bool retirementPending(){return false;}}}
void tick();void resumeAfterMusicAudio();AudioSuspension suspendForMusicAudio(uint32_t);
bool queueReady=true,worker=false,startOk=true,pollerRunning=false;
unsigned starts=0;std::function<void()> duringStart;
struct Queue {bool workerRunning(){return worker;}bool resumeWorker(){return true;}bool stopWorkerForVoice(uint32_t){return true;}} queue;
namespace services {
namespace net_poll {
 bool stop(uint32_t){pollerRunning=false;return true;}
 bool start(){++starts;if(startOk)pollerRunning=true;if(duringStart){auto f=duringStart;duringStart=nullptr;f();}return startOk;}
}}
namespace uploads {Queue* readyQueue(){return queueReady?&queue:nullptr;}}
'''
code+=re.search(r'^(?:volatile bool|std::atomic<[^>]+>) s_restartNetPoll[^\n]+',s,re.M)[0]+'\n'
if 'void setPollerRestart(bool requested)' in s:code+=function('void setPollerRestart(bool requested)')+'\n'
for sig in ['void tick()', 'void resumeAfterMusicAudio()', 'AudioSuspension suspendForMusicAudio(uint32_t timeoutMs)']:
 code+=function(sig)+'\n'
code+=r'''
int main(){
 tick();assert(starts==0);
 resumeAfterMusicAudio();resumeAfterMusicAudio();worker=true;tick();assert(starts==0);
 worker=false;startOk=false;tick();assert(starts==1);
 startOk=true;tick();assert(starts==2);tick();assert(starts==2);
 resumeAfterMusicAudio();assert(suspendForMusicAudio(30).uploadsReady);tick();assert(starts==2);
 // Music stops the newly started poller and releases while startup returns.
 // Acknowledging the old intent must not leave the stopped poller without a retry.
 duringStart=[] {assert(suspendForMusicAudio(30).uploadsReady);resumeAfterMusicAudio();};resumeAfterMusicAudio();tick();
 const auto beforeSecond=starts;tick();
'''
if a.negative_control:code+='assert(starts==beforeSecond&&!pollerRunning);puts("REPRODUCED: startup acknowledgement erased a newer restart request");\n'
else:code+='assert(starts==beforeSecond+1&&pollerRunning);puts("PASS newer restart request survives startup acknowledgement");\n'
if not a.negative_control:
 code+='s_restartNetPoll.store(0xfffffffeu);resumeAfterMusicAudio();auto wrap=starts;tick();assert(starts==wrap+1);tick();assert(starts==wrap+1);\n'
code+=r'''
 // Cancellation must survive both successful and failed startup returns.
 for(bool success:{false,true}){
  startOk=success;duringStart=[] {assert(suspendForMusicAudio(30).uploadsReady);};
  resumeAfterMusicAudio();tick();const auto cancelled=starts;startOk=true;
  tick();assert(starts==cancelled);
 }
 // Last intent wins: release followed by acquire cancels the pending retry.
 duringStart=[] {resumeAfterMusicAudio();assert(suspendForMusicAudio(30).uploadsReady);};
 resumeAfterMusicAudio();tick();auto settled=starts;tick();assert(starts==settled);
 // Music release before queue initialization still requests poller restoration.
 queueReady=false;resumeAfterMusicAudio();tick();assert(starts==settled+1);queueReady=true;
 std::atomic<bool> go{false};
 std::thread publisher([&]{while(!go.load())std::this_thread::yield();for(unsigned i=0;i<100000;++i)resumeAfterMusicAudio();});
 go=true;for(unsigned i=0;i<100000;++i)tick();publisher.join();tick();
 settled=starts;tick();assert(starts==settled);
 puts("PASS coalescing, idle deferral, retry, cancellation, optional queue and 100000 concurrent publications");
}
'''
with tempfile.TemporaryDirectory() as directory:
 r=Path(directory);(r/'probe.cpp').write_text(code)
 subprocess.run(['c++','-std=c++11','-pthread','-fsanitize=thread','-g',str(r/'probe.cpp'),'-o',str(r/'probe')],check=True)
 result=subprocess.run([str(r/'probe')],capture_output=True,text=True)
 if a.negative_control:
  assert 'REPRODUCED: startup acknowledgement erased' in result.stdout,result.stdout+result.stderr
  assert result.returncode and 'ThreadSanitizer: data race' in result.stderr,result.stderr
  print('PASS negative control: newer request loss reproduced and volatile publication reports a TSan race')
 else:
  if result.returncode:raise RuntimeError(result.stdout+result.stderr)
  print(result.stdout.strip())
