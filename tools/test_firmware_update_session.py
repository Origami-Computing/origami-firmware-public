"""Exercise OTA ownership without downloading or activating firmware."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    def write(name, value):
        p = root/name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(value)
    write('services/background_network.h', '#include <stdint.h>\nnamespace services {namespace background_network {bool suspendForPeripheralIo(uint32_t);void resumeAfterPeripheralIo();extern bool httpBusy;class PeriodicWork{bool held=false;public:bool tryEnter(){if(httpBusy)return false;return held=httpBusy=true;}void release(){if(held){held=false;httpBusy=false;}}};}}')
    write('services/ota_client.h', (ROOT/'include/services/ota_client.h').read_text())
    write('Arduino.h', '#pragma once\n#include <stdint.h>\nextern uint32_t clockMs;inline uint32_t millis(){return clockMs;}inline void vTaskDelete(void*){}\nstruct Logger {template<class... T> void printf(const char*,T...){} void println(const char*){}};extern Logger Serial;')
    write('hal/hal_wifi.h', '#pragma once\nextern bool wifi;namespace hal {namespace wifi {inline bool connected(){return ::wifi;}}}')
    write('hal/hal_tasks.h', '#pragma once\nextern void(*pendingWorker)(void*);extern bool spawnOk;namespace hal {namespace tasks {enum class Id {OtaNotice};inline void recordCurrentStack(Id){}inline void* spawn(Id,void(*fn)(void*)){if(!spawnOk)return nullptr;pendingWorker=fn;return (void*)1;}}}')
    write('core/memory.h', 'namespace core {namespace memory {void logSnapshot(const char*);}}')
    write('main.cpp', r'''
#include "services/firmware_update.h"
#include <cassert>
#include <Arduino.h>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <string>
using Session=services::firmware_update::Session;
using Result=Session::Result;
std::string trace;
Logger Serial;
uint32_t clockMs=1000;
bool wifi=true,spawnOk=true,fetchOk=true;
int fetches=0;
void(*pendingWorker)(void*)=nullptr;
using Channel=services::ota_client::ReleaseChannel;
Channel channel=Channel::Production;
bool backgroundOk=true,agentOk=true,background=false,agent=false,worker=false;
namespace services {
namespace background_network {
bool httpBusy=false;
bool suspendForPeripheralIo(uint32_t ms){assert(ms==1500);trace+='p';background=backgroundOk;return backgroundOk;}
void resumeAfterPeripheralIo(){assert(background&&!agent&&!worker);trace+='P';background=false;}
}
namespace agent_coordination {
bool TransportPause::acquire(uint32_t ms){if(held_)return true;assert(ms==15000&&background);trace+='a';agent=agentOk;return held_=agentOk;}
void TransportPause::release(){if(!held_)return;held_=false;assert(agent&&background&&!worker);trace+='A';agent=false;}
}}
namespace services {namespace ota_client {
void abortFlash(){trace+='x';worker=false;}
ReleaseChannel releaseChannel(){return channel;}
const char* releaseChannelName(ReleaseChannel){return "test";}
bool isReleaseNewer(ReleaseChannel,const char* tag){return !strcmp(tag,"new");}
bool fetchLatestRelease(ReleaseChannel c,ReleaseInfo* out){assert(!background&&!agent);++fetches;out->channel=c;strcpy(out->tag,"new");return fetchOk;}
}}
namespace core {
namespace memory {void logSnapshot(const char* label){assert(std::string(label)=="ota.network.reserved");trace+='l';}}
}
int main(){
 {
  Session session;backgroundOk=false;
  assert(session.reserveNetwork()==Result::BackgroundBusy);assert(trace=="p");
  assert(!background&&!agent);trace.clear();backgroundOk=true;agentOk=false;
  assert(session.reserveNetwork()==Result::AgentBusy);assert(trace=="paP");
  assert(!background&&!agent);trace.clear();agentOk=true;
  assert(session.reserveNetwork()==Result::Ready);assert(trace=="pal");
  trace.clear();assert(session.reserveNetwork()==Result::Ready);assert(trace.empty());
  worker=true;session.cancel();assert(trace=="x"&&background&&agent&&!worker);
  trace.clear();worker=true;session.close();assert(trace=="xAP");
  trace.clear();session.close();assert(trace=="x");
  trace.clear();assert(session.reserveNetwork()==Result::Ready);worker=true;
  trace.clear();
 }
 assert(trace=="xAP"&&!worker&&!background&&!agent);
 namespace update=services::firmware_update;
 trace.clear();wifi=false;update::checkSettingsNotice();assert(!pendingWorker&&trace.empty());wifi=true;
 services::background_network::httpBusy=true;
 update::checkSettingsNotice();assert(!pendingWorker&&!update::noticeBusy()&&fetches==0);
 services::background_network::httpBusy=false;
 update::checkSettingsNotice();assert(update::noticeBusy()&&!background&&!agent&&pendingWorker&&trace.empty());
 assert(update::noticeInFlight());
 Session foreground;assert(foreground.reserveNetwork()==Result::BackgroundBusy);
 update::checkSettingsNotice();assert(fetches==0); // UI queues; no synchronous request.
 pendingWorker(nullptr);pendingWorker=nullptr;
 update::pollSettingsNotice();assert(!update::noticeBusy()&&!background&&!agent);
 assert(!services::background_network::httpBusy);
 assert(update::settingsUpdateAvailable());
 services::ota_client::ReleaseInfo cached{};assert(update::copySettingsRelease(&cached)&&!strcmp(cached.tag,"new"));
 update::checkSettingsNotice();assert(!pendingWorker&&fetches==1);
 channel=Channel::Staging;assert(!update::settingsUpdateAvailable());
 update::checkSettingsNotice();pendingWorker(nullptr);pendingWorker=nullptr;update::pollSettingsNotice();
 assert(update::settingsUpdateAvailable()&&fetches==2);
 update::invalidateSettingsNotice();spawnOk=false;update::checkSettingsNotice();
 assert(!services::background_network::httpBusy);
 assert(!update::noticeBusy()&&!pendingWorker&&fetches==2); // Low memory NEVER runs HTTPS on UI.
 clockMs+=29999;update::checkSettingsNotice();assert(!pendingWorker);
 clockMs+=1;spawnOk=true;fetchOk=false;update::checkSettingsNotice();assert(pendingWorker);
 pendingWorker(nullptr);pendingWorker=nullptr;assert(!update::settingsUpdateAvailable()&&fetches==3);
 clockMs+=30000;fetchOk=true;update::checkSettingsNotice();pendingWorker(nullptr);pendingWorker=nullptr;
 assert(update::settingsUpdateAvailable()&&fetches==4);
 update::invalidateSettingsNotice();foreground.reserveNetwork();update::checkSettingsNotice();assert(!pendingWorker);foreground.close();
 // Avoid test-fixture destructor ordering.
 // All owned resources are already closed; bypass unrelated process-exit teardown.
 std::_Exit(0);
}
''')
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(root),'-I'+str(ROOT/'include'),str(ROOT/'src/services/firmware_update.cpp'),str(root/'main.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
print('PASS OTA resource timeouts, rollback, retry, cancellation retention, ordered close and destructor cleanup')
