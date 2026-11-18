"""Actual file-sync request/status/poll bodies with offline JSON and transport fakes.

--expect-bug characterizes frozen build158 request/status ordering gaps. --thread-sanitizer
also exercises concurrent UI publication/status reads and worker polling. This
never mounts SD, contacts a server or executes remote file mutations.
"""
from pathlib import Path
import argparse,re,subprocess,tempfile,hashlib,json
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--expect-bug',action='store_true')
p.add_argument('--thread-sanitizer',action='store_true')
p.add_argument('--negative-control',action='store_true',help='Alias for --expect-bug against frozen build158')
a=p.parse_args()
a.expect_bug = a.expect_bug or a.negative_control
s=(ROOT/'src/services/device_fs.cpp').read_text()
header=(ROOT/'include/services/device_fs.h').read_text()
if a.expect_bug:
 fixture=ROOT/'test/fixtures/device_sync_handoff'
 manifest=json.loads((fixture/'manifest.json').read_text())
 for name,digest in manifest['files'].items():
  assert hashlib.sha256((fixture/name).read_bytes()).hexdigest()==digest
 s=(fixture/'device_fs.cpp').read_text();header=(fixture/'device_fs.h').read_text()
def function(prefix):
 start=s.index(prefix);end=s.index('{',start)+1;depth=1
 while depth:depth+=(s[end]=='{')-(s[end]=='}');end+=1
 return s[start:end]
code=r'''
#include "services/device_fs.h"
#include <cassert>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>
#include <vector>
#include <string>
#define LOGI(...) ((void)0)
#define LOGV(...) ((void)0)
#define LOGW(...) ((void)0)
using portMUX_TYPE=std::mutex;
#define portMUX_INITIALIZER_UNLOCKED {}
thread_local std::function<void()> beforeEnter;
thread_local unsigned lockDepth=0;
void portENTER_CRITICAL(portMUX_TYPE* m){if(beforeEnter){auto f=beforeEnter;beforeEnter=nullptr;f();}m->lock();++lockDepth;}
void portEXIT_CRITICAL(portMUX_TYPE* m){--lockDepth;m->unlock();}
uint32_t millis(){return 42;}
size_t strlcpy(char* dest,const char* src,size_t n){auto len=strlen(src);if(n){auto copied=len<n-1?len:n-1;memcpy(dest,src,copied);dest[copied]=0;}return len;}
bool registered=true,wakeOk=true;unsigned wakes=0,manifests=0;int postCode=200,getCode=200;
bool nullOps=true;unsigned serverPending=0;
namespace core {
 namespace secrets {bool hasDeviceToken(){return registered;}}
 namespace watchdog {void feed(){}}
}
namespace services {namespace net_poll {bool stopRequested(){return false;}bool start(){assert(!lockDepth);++wakes;return wakeOk;}}}
struct Value {const char* text=nullptr;template<class T>T as(){return T{};}template<class T>T to(){return T{};}template<class T>Value& operator=(T){return *this;}template<class T>T operator|(T value){return value;}const char* operator|(const char* fallback){return text?text:fallback;}unsigned operator|(unsigned){return serverPending;}};
struct JsonObject {const char* id="",*type="",*path="",*url="";Value operator[](const char* key){return {strcmp(key,"id")==0?id:strcmp(key,"op")==0?type:strcmp(key,"path")==0?path:strcmp(key,"url")==0?url:nullptr};}};
std::vector<JsonObject> serverOps;
struct JsonArray {
 bool isNull(){return nullOps;}unsigned size(){return serverOps.size();}
 auto begin(){return serverOps.begin();}auto end(){return serverOps.end();}
};
char s_jsonMemory; // Offline JSON fake: allocation policy is tested separately.
struct JsonDocument {explicit JsonDocument(void* = nullptr){}Value operator[](const char*){return {};}};
constexpr const char* DEVICE_FS_OPS_PATH="/offline";
int httpGetJson(const char*,JsonDocument&){assert(!lockDepth);return getCode;}
using String=std::string;
constexpr const char* DEVICE_FS_MANIFEST_PATH="/offline-manifest";
constexpr unsigned DEVICE_FS_MANIFEST_MAX=100;
void serializeJson(JsonDocument&,String&){}
int httpPostJson(const char*,const String&){assert(!lockDepth);return postCode;}
bool ok2xx(int code){return code>=200&&code<300;}
std::vector<std::pair<unsigned,int>> observedTransfers;
void doPush(const char*,const char*){
#if EXPECT_BUG
 assert(false);
#else
 services::device_fs::SyncStatus s;services::device_fs::getSyncStatus(s);
 observedTransfers.push_back({s.remaining,int(s.direction)});
#endif
}
void doPull(const char*,const char*,const char*){
#if EXPECT_BUG
 assert(false);
#else
 services::device_fs::SyncStatus s;services::device_fs::getSyncStatus(s);
 observedTransfers.push_back({s.remaining,int(s.direction)});
#endif
}
void doDelete(const char*,const char*){assert(false);}void doMkdir(const char*,const char*){assert(false);}void doList(const char*,const char*){assert(false);}
void reportResult(const char*,bool,unsigned,const char*){assert(false);}
thread_local std::function<void()> duringManifest;
bool walkInto(const char*,JsonArray&,int& count){assert(!lockDepth);++manifests;count=3;if(duringManifest){auto f=duringManifest;duringManifest=nullptr;f();}return true;}
namespace services {namespace device_fs {
'''
start=re.search(r'static (?:volatile )?bool s_syncRequested',s).start();end=s.index('\n\nvoid setStatus',start)
code+=s[start:end]+'\n'
if not a.expect_bug:code+=function('void setStatusLocked(')+'\n'
code+=function('void setStatus(')+'\n'
if not a.expect_bug:code+=function('void finishPollStatus(')+'\n'
for sig in ['void doManifest(', 'bool poll()', 'void requestSync(', 'void getSyncStatus(']:code+=function(sig)+'\n'
code+=r'''
void check(bool bug,const char* label){
#if EXPECT_BUG
 assert(bug);printf("REPRODUCED %s\n",label);
#else
 assert(!bug);printf("PASS protected against: %s\n",label);
#endif
}
void test(){
 SyncStatus status;
 requestSync();duringManifest=[] {requestSync();};assert(poll());getSyncStatus(status);
 check(s_syncRequested&&status.done&&!status.active,"older poll marks sync complete while a newer manifest request remains pending");
 poll(); // Drain that pending request before the next independent schedule.
 beforeEnter=[] {assert(poll());};requestSync();getSyncStatus(status);
 check(!s_syncRequested&&status.active&&!strcmp(status.detail,"Starting..."),"request publishes before Starting status; completed work is overwritten as active");
#if !EXPECT_BUG
 poll();getSyncStatus(status);assert(status.done&&!status.active&&!s_syncRequested);
 auto before=manifests;requestSync();requestSync();assert(wakes==0);assert(poll()&&manifests==before+1);
 registered=false;requestSync();assert(!poll()&&s_syncRequested);registered=true;assert(poll()&&!s_syncRequested);
 requestSync(SyncRequest::Interactive);assert(wakes==1&&s_syncRequested);poll();
 wakeOk=false;requestSync(SyncRequest::Interactive);assert(wakes==2&&s_syncRequested);poll();wakeOk=true;
 puts("PASS request coalescing, registration deferral, interactive wake and queue-only activation");
 for(int code:{-1,401,408,429,500,503,400,403,404}){
  postCode=code;requestSync();poll();
  const bool retry=code<=0||code>=500||code==401||code==408||code==429;
  assert(s_syncRequested==retry);getSyncStatus(status);assert(status.active==retry);
  postCode=200;if(retry)poll();
  postCode=code;doManifest("server-operation");assert(!s_syncRequested); // Server lease, no local retry.
  postCode=200;
 }
 // Older terminal transfer statuses cannot complete newly queued local work.
 requestSync();setStatus("Upload failed","old operation",0,1,false,true,false);
 getSyncStatus(status);assert(status.active&&!status.done);poll();
 nullOps=false;serverPending=2;poll();getSyncStatus(status);assert(status.active&&status.max==2);
 serverPending=0;poll();getSyncStatus(status);assert(status.done&&!status.active);nullOps=true;
 puts("PASS existing retry classes, server lease behavior, pending completion guard and empty-poll status");
 nullOps=false;serverOps={{"p1","push","/notes/a.txt",""},{"p2","pull","/notes/b.txt","/signed"}};
 assert(poll());getSyncStatus(status);
 assert(observedTransfers.size()==2&&observedTransfers[0]==std::make_pair(2u,1)&&
        observedTransfers[1]==std::make_pair(1u,2));
 assert(status.remaining==0&&status.direction==SyncStatus::Direction::None);
 serverOps.clear();nullOps=true;
 puts("PASS upload/download direction and remaining-operation publication");
 requestSync();getCode=503;assert(!poll());getSyncStatus(status);
 assert(status.done&&!status.ok&&status.remaining==1);
 getCode=200;requestSync();assert(poll());getSyncStatus(status);
 assert(status.ok&&status.remaining==0);
 puts("PASS failed operation fetch preserves pending sync and a fresh request recovers");
#endif
#if THREAD_STRESS
 std::atomic<bool> go{false};
 std::thread ui([&]{while(!go.load()){}for(unsigned i=0;i<100000;++i){requestSync();SyncStatus copy;getSyncStatus(copy);}});
 go=true;for(unsigned i=0;i<100000;++i)poll();ui.join();poll();
 // Isolate status publication from the legacy request's full memory fence.
 // Both sides still execute the real status setter and real polling reader.
 std::atomic<bool> statusGo{false};
 std::thread publisher([&]{while(!statusGo.load()){}for(unsigned i=0;i<100000;++i)setStatus("Syncing","Starting...",0,1);});
 statusGo=true;for(unsigned i=0;i<100000;++i)poll();publisher.join();
#endif
}
}}
int main(){services::device_fs::test();}
'''
with tempfile.TemporaryDirectory() as d:
 r=Path(d);(r/'test.cpp').write_text(code)
 (r/'services').mkdir();(r/'services/device_fs.h').write_text(header)
 subprocess.run(['c++','-std=c++14','-pthread','-g','-fsanitize='+('thread' if a.thread_sanitizer else 'address,undefined'),'-DEXPECT_BUG='+str(int(a.expect_bug)),'-DTHREAD_STRESS='+str(int(a.thread_sanitizer)),'-I'+str(r),'-I'+str(ROOT/'include'),str(r/'test.cpp'),'-o',str(r/'test')],check=True)
 result=subprocess.run([str(r/'test')],capture_output=True,text=True,timeout=30)
 if a.expect_bug and a.thread_sanitizer:
  assert result.returncode and 'ThreadSanitizer: data race' in result.stderr,result.stdout+result.stderr
  assert result.stdout.count('REPRODUCED')==2,result.stdout
  locations=sorted(set(re.findall(r"Location is global '([^']+)'",result.stderr)))
  assert any('s_syncRequested' in x for x in locations),result.stderr
  assert any('s_status' in x for x in locations),result.stderr
  print(result.stdout.strip());print('REPRODUCED ThreadSanitizer races: '+', '.join(locations))
 else:
  print(result.stdout.strip())
  if result.returncode:raise RuntimeError(result.stderr)
