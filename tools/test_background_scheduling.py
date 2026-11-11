"""Verify real svcTask scheduling through published health permissions.

Fake peripherals/tasks isolate admission, OTA and upload scheduling. This does
not prove thread safety, transport quiescence or device/network behavior.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/services.cpp').read_text()
a = source.index('static void svcTask(void*) {')
b = source.index('{', a) + 1
depth = 1
while depth:
    depth += (source[b] == '{') - (source[b] == '}')
    b += 1
function = source[a:b]
assert "NavHandler" not in function and "apps::" not in function, "Worker must consume health permissions, not UI state"
agent_source = (ROOT/'src/apps/agent_app.cpp').read_text()
a = agent_source.index('static core::health::RuntimePolicy runtimePolicy() {')
b = agent_source.index('\n}', a)+2
agent_policy = agent_source[a:b]
code = r'''
#include <cassert>
#include "core/health_policy.h"
#include "core/release_policy.h"
#include <cstdint>
#include <string>
#include <vector>
#include <stdexcept>
#include <thread>
#include <atomic>
constexpr uint32_t OTA_AUTO_CHECK_INTERVAL_MS=1000;
#define pdMS_TO_TICKS(x) (x)
std::atomic<bool> s_updatePrompt{false};
uint32_t s_uploadTickMs=0;
enum Stage {Eyes,Recording,Thinking,Reply,Error};
namespace motion {void beforeStage(Stage,Stage){}}
struct Frame {
 uint32_t now; Stage prevStageForIdle=Eyes; Stage stage=Eyes; int depth=1;
 bool wifi=true,hold=false,remote=false,admit=true,delegated=false;
 Frame(uint32_t t,Stage s=Eyes,int d=1,bool w=true,bool h=false,bool r=false,bool a=true)
 :now(t),stage(s),depth(d),wifi(w),hold(h),remote(r),admit(a){}
};
std::vector<Frame> frames;size_t next=0;Frame current(0);
int feeds=0,quotes=0,clears=0,ensures=0,ticks=0,releases=0,networkTicks=0;
std::vector<std::string> events;
struct Stop{};
void publishFrame();
void vTaskDelay(int ms){assert(ms==20);if(next==frames.size())throw Stop{};current=frames[next++];publishFrame();}
uint32_t millis(){return current.now;}
struct SerialMock {void println(const char*){events.push_back("held");}} Serial;

namespace apps {namespace agent {
 namespace flow {bool waitingForDelegation(){return current.delegated;}}
 bool isAtHomescreen(){return current.stage==Eyes;}
 bool isRecording(){return current.stage==Recording;}
 bool isSending(){return current.stage==Thinking;}
 bool isOnReplyScreen(){return current.stage==Reply;}
 bool isShowingError(){return current.stage==Error;}
}}
namespace hal {namespace wifi {bool connected(){return current.wifi;}}}
namespace core {
namespace watchdog {void subscribe(){}void feed(){++feeds;}}

}
namespace services {
// Latency telemetry is independent of the scheduling policy under test.
namespace agent_latency {void fail(){} void cancel(){} void replyReady(){}}
namespace automation {
 bool otaUpdateRequested(){return current.remote;}
 void clearOtaRequest(){++clears;events.push_back("clear");}
}
namespace background_network {struct ServiceWork {
 explicit operator bool()const{return current.admit;}
 ~ServiceWork(){++releases;}
};
void tick(){
 assert(current.admit && core::health::backgroundPolicy().allowUploads);
 assert(ticks==networkTicks+1 && ensures==ticks);
 assert(!events.empty() && events.back()=="tick");
 ++networkTicks;events.push_back("network-tick");
}}

namespace morning_quote {void serviceBackground(uint32_t){++quotes;events.push_back("quote");}}
namespace uploads {
 void ensureInit(){++ensures;events.push_back("ensure");}
 void tick(){++ticks;events.push_back("tick");}
}}
'''
code += 'namespace apps {namespace agent {' + agent_policy + '}}\n'
code += 'void publishFrame(){core::health::publishBackgroundPolicy(apps::agent::runtimePolicy(),current.depth);}\n'
flow_source=(ROOT/'src/apps/agent/agent_flow.cpp').read_text()
a=flow_source.index('void goTo(Stage next) {')
b=flow_source.index('\n}',a)+2
code += r'''
#define LOGD(...) ((void)0)
Frame& s_state=current;
struct NavHandler {static NavHandler& instance(){static NavHandler nav;return nav;}void runtimePolicyChanged(){publishFrame();}};
// Home setup is tested separately; this harness isolates policy publication.
void prepareHome() {}
int app=0,screen=0,screenSwitches=0;
int& appScreenFor(Stage){return screen;}
namespace scene {enum class ScreenTransition {Back,Forward};void switchTo(int*,int*,ScreenTransition){
 ++screenSwitches;
 const auto snapshot=core::health::backgroundPolicy();
 assert(snapshot.allowUploads==(current.stage!=Recording&&current.stage!=Thinking));
 assert(snapshot.allowAutomaticUpdate==(current.depth==1&&current.stage==Eyes));
}}
'''
code += flow_source[a:b]
code += function
code += r'''
int main(int argc,char** argv){
 assert(argc==2);std::string name=argv[1];
 if(name=="transitions"){
  for(Stage before:{Eyes,Recording,Thinking,Reply,Error})for(Stage after:{Eyes,Recording,Thinking,Reply,Error}){
   current.stage=before;publishFrame();int old=screenSwitches;goTo(after);
   assert(current.stage==after&&screenSwitches==old+(before!=after));
  }return 0;
 }
 if(name=="atomic"){
  core::health::RuntimePolicy policy;core::health::publishBackgroundPolicy(policy,1);
  std::atomic<bool> done{false};
  std::thread reader([&]{while(!done.load()){auto p=core::health::backgroundPolicy();assert(p.allowUploads==p.allowAutomaticUpdate);}});
  for(int n=0;n<100000;++n){policy.allowBackgroundUploads=policy.allowAutomaticUpdate=bool(n%2);core::health::publishBackgroundPolicy(policy,1);}
  done.store(true);reader.join();return 0;
 }
 int expectedTicks=0,expectedQuotes=0,expectedClears=0;
 bool expectedOta=false;
 if(name=="delegated"){
  frames={{250,Eyes},{500,Eyes},{750,Eyes}};
  frames[0].delegated=frames[1].delegated=true;
  expectedTicks=1;expectedQuotes=3;
 }else if(name=="stages"){
  // Reply/Error resume uploads; only Recording/Thinking suppress the kick.
  frames={{250,Eyes},{500,Recording},{750,Thinking},{1000,Reply},{1250,Error}};
  expectedTicks=3;expectedQuotes=5;
 }else if(name=="covered-sync"){
  // A default-permission app such as Photos keeps the existing upload cadence
  // while mounted over Home. It must not bypass an active Agent's constraint.
  frames={{250,Eyes,2},{500,Eyes,3},{750,Thinking,3},{1000,Reply,3}};
  expectedTicks=3;expectedQuotes=4;
 }else if(name=="cadence"){
  frames={{249},{250},{251},{499},{500}};expectedTicks=2;expectedQuotes=5;
 }else if(name=="pause"){
  frames={{100,Eyes,1,true,false,true,false},{1100,Eyes,1,true,false,true,false}};
 }else if(name=="resume"){
  frames={{100,Eyes,1,true,false,false,false},{1100}};
  expectedTicks=1;expectedQuotes=1; // First eligible instant starts OTA timer.
 }else if(name=="ota"){
  frames={{100},{1099},{1100}};expectedOta=true;expectedTicks=1;expectedQuotes=3;
 }else if(name=="ota-gap"){
  // Time spent covered/offline is not subtracted or reset when Home returns.
  frames={{100},{500,Reply,2,false},{1100}};
  expectedOta=true;expectedTicks=2;expectedQuotes=3;
 }else if(name=="remote"){
  // Existing remote request bypasses Home/depth/connectivity/PTT constraints.
  frames={{100,Recording,4,false,false,true}};
  expectedOta=true;expectedClears=1;expectedQuotes=1;
 }else if(name=="held-remote"){
  frames={{100,Recording,4,false,true,true}};
  expectedOta=true;expectedClears=1;expectedQuotes=1; // Legacy hold no longer suppresses confirmation.
 }else if(name=="pending"){
  s_updatePrompt.store(true);
  frames={{250,Eyes,1,true,false,true}};
  expectedOta=true;expectedTicks=1;expectedQuotes=1; // Remote remains unconsumed.
 }else if(name=="wrap"){
  s_uploadTickMs=UINT32_MAX-100;
  frames={{UINT32_MAX-500},{600}};
  expectedOta=true;expectedTicks=2;expectedQuotes=2;
 }else if(name=="ineligible"){
  frames={{100,Reply},{1200,Eyes,2},{2300,Eyes,1,false},{3400,Eyes,1,true,true},{4500}};
  expectedOta=true;expectedTicks=4;expectedQuotes=5; // Legacy hold no longer suppresses scheduled prompts.
 }else {assert(false);}
 if(core::release_policy::productionOnly){
  if(name!="pending")expectedOta=false;
  if(name=="pending")expectedClears=1;
 }
 try{svcTask(nullptr);}catch(const Stop&){}
 assert(feeds==int(frames.size())&&releases==feeds);
 assert(quotes==expectedQuotes&&clears==expectedClears);
 assert(ticks==expectedTicks&&ensures==ticks&&networkTicks==ticks);
 assert(s_updatePrompt.load()==expectedOta);
 if(name=="held-remote")assert((events==(core::release_policy::productionOnly ? std::vector<std::string>{"quote","clear","held"} : std::vector<std::string>{"quote","clear"})));
 if(name=="pending")assert((events==(core::release_policy::productionOnly
   ?std::vector<std::string>{"quote","clear","held","ensure","tick","network-tick"}
   :std::vector<std::string>{"quote","ensure","tick","network-tick"})));
}
'''
with tempfile.TemporaryDirectory() as directory:
    path = Path(directory)
    (path/'test.cpp').write_text(code)
    binary = path/'test'
    for production in (0,1):
      subprocess.run(['c++', '-std=c++11', '-pthread', '-fsanitize=address,undefined',f'-DORIGAMI_PRODUCTION_BUILD={production}',
                    '-I'+str(ROOT/'include'), str(ROOT/'src/core/health_policy.cpp'), str(path/'test.cpp'), '-o', str(binary)], check=True)
      for case in ('delegated','stages','covered-sync','cadence','pause','resume','ota','ota-gap','remote',
                 'held-remote','pending','wrap','ineligible','transitions','atomic'):
        subprocess.run([str(binary), case], check=True)
print('PASS 13 worker scenarios including delegated replies, 25 actual Agent transitions and coherent atomic snapshots under ASAN/UBSAN')
