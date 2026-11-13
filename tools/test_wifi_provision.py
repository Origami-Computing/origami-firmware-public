"""Exercise the production provisioning owner; radio work must remain deferred."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory)
 def write(name,text):
  target=p/name;target.parent.mkdir(parents=True,exist_ok=True);target.write_text(text)
 write('Arduino.h','#pragma once\nstruct Log {void println(const char*){}};inline Log Serial;\n')
 write('WiFi.h','#pragma once\nenum wifi_mode_t {WIFI_MODE_STA=1,WIFI_MODE_APSTA=3};\n')
 write('core/secrets.h',r'''#pragma once
#include <cstring>
namespace core {namespace secrets {
inline bool saveOk=true,removeOk=true;inline int saves=0,removes=0;
inline char stored[33]="selected",last[33]="";
inline bool addWifiNetwork(const char* s,const char*){++saves;if(saveOk)strcpy(stored,s);return saveOk;}
inline bool promoteWifiNetworkToFront(const char*){strcpy(stored,"reordered");return true;}
inline bool setLastConnectedWifi(const char* s){strcpy(last,s);return true;}
inline bool setApiKey(const char*){return true;}
inline bool removeWifiNetwork(int){++removes;return removeOk;}
}}
''')
 write('services/background_network.h',r'''#pragma once
namespace services {namespace background_network {
inline bool allowPause=true,held=false;inline int releases=0;
struct RadioPause {enum class Result {Acquired,Busy};Result acquire(){if(!allowPause)return Result::Busy;held=true;return Result::Acquired;}void release(){held=false;++releases;}};
}}
''')
 write('hal/hal_wifi.h',r'''#pragma once
#include <WiFi.h>
namespace hal {namespace wifi {
inline bool changing=false;inline int requests=0,changes=0;inline wifi_mode_t mode=WIFI_MODE_STA;
inline void requestConnect(wifi_mode_t m){++requests;mode=m;}
inline void requestCredentialsChanged(){++changes;changing=true;}
inline bool credentialsChangePending(){return changing;}
}}
''')
 write('test.cpp',r'''
#include "services/wifi_provision.h"
#include "core/secrets.h"
#include "hal/hal_wifi.h"
#include "services/background_network.h"
#include <cassert>
int main(){
 namespace p=services::wifi_provision;namespace w=hal::wifi;namespace s=core::secrets;
 p::Credentials c{};assert(!p::saveAndConnect(c)&&w::requests==0);
 strcpy(c.ssid,"test");strcpy(c.pass,"fixture-only");s::saveOk=false;
 assert(!p::saveAndConnect(c)&&w::requests==0);s::saveOk=true;
 assert(p::saveAndConnect(c)&&w::requests==1&&w::mode==WIFI_MODE_STA);
 assert(p::saveAndConnect(c,true)&&w::requests==2&&w::mode==WIFI_MODE_APSTA);
 strcpy(s::stored,"selected");assert(p::connectSaved(s::stored));assert(!strcmp(s::last,"selected"));
 assert(!p::connectSaved(nullptr));char longName[34];memset(longName,'x',33);longName[33]=0;assert(!p::connectSaved(longName));
 namespace n=services::background_network;
 n::allowPause=false;assert(!p::forgetSaved(0)&&s::removes==0&&w::changes==0);n::allowPause=true;
 s::removeOk=false;assert(!p::forgetSaved(0)&&w::changes==0&&!n::held);
 s::removeOk=true;assert(p::forgetSaved(0)&&w::changes==1&&n::held);
 p::service();assert(n::held);assert(!p::forgetSaved(0));assert(!p::saveAndConnect(c));assert(!p::connectSaved("test"));
 w::changing=false;p::service();assert(!n::held);int releases=n::releases;p::service();assert(n::releases==releases);
 assert(p::saveAndConnect(c));
}
''')
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(p),'-I'+str(ROOT/'include'),str(ROOT/'src/services/wifi_provision.cpp'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
# USB adapters must use this same owner, never mutate the manager cache/radio.
s=(ROOT/'src/tooling/dev_serial_input.cpp').read_text()
a=s.index('static bool connectSavedWifi()');b=s.index('static void printBackendUsage()',a)
assert 'hal::wifi::reloadCredentialsFromSecrets' not in s[a:b]
assert 'hal::wifi::setMode' not in s[a:b]
assert s[a:b].count('services::wifi_provision::saveAndConnect(credentials)')==2
print('PASS provisioning failure, selected-name alias, deferred join/forget and both USB input formats')

# Compile the actual manager completion boundary: pending must remain published
# during every driver/cache operation and clear on all early-return paths.
hal=(ROOT/'src/hal/hal_wifi.cpp').read_text()
block=hal[hal.index('void requestCredentialsChanged()'):hal.index('void setWifiRestoreDeferred')]
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory)
 (p/'manager.cpp').write_text(r'''#include <atomic>
#include <cassert>
#include <cstdio>
std::atomic<bool> s_credentialsChangedPending{false};
bool s_initialized=false,s_wifiSuspended=false,modeOk=true;int s_credCount=0,reloads=0,joins=0,offs=0;
constexpr int WIFI_OFF=0,WIFI_MODE_STA=1;
#define pdMS_TO_TICKS(x) (x)
enum class RadioState {Suspended,OfflineBle,RestoringWifi};
RadioState state=RadioState::Suspended;
void setRadioState(RadioState s,const char*){assert(s_credentialsChangedPending.load());state=s;}
void reloadCredentialsFromSecrets(){assert(s_credentialsChangedPending.load());++reloads;}
bool setMode(int){assert(s_credentialsChangedPending.load());return modeOk;}
void connectNow(){assert(s_credentialsChangedPending.load());++joins;}
void vTaskDelay(int){assert(s_credentialsChangedPending.load());}
struct {void println(const char*){}} Serial;
struct {void setAutoReconnect(bool){}void disconnect(bool){assert(s_credentialsChangedPending.load());++offs;}void mode(int){assert(s_credentialsChangedPending.load());}} WiFi;
'''+block+r'''
int main(){
 requestCredentialsChanged();servicePendingCredentialsChanged();assert(!credentialsChangePending()&&!offs);
 s_initialized=true;s_wifiSuspended=true;requestCredentialsChanged();servicePendingCredentialsChanged();assert(!credentialsChangePending()&&reloads==1&&!offs);
 s_wifiSuspended=false;requestCredentialsChanged();servicePendingCredentialsChanged();assert(!credentialsChangePending()&&offs==1&&state==RadioState::OfflineBle);
 s_credCount=1;requestCredentialsChanged();servicePendingCredentialsChanged();assert(!credentialsChangePending()&&joins==1&&!s_wifiSuspended);
 modeOk=false;requestCredentialsChanged();servicePendingCredentialsChanged();assert(!credentialsChangePending()&&joins==1&&s_wifiSuspended);
}
''')
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined',str(p/'manager.cpp'),'-o',str(p/'manager')],check=True)
 subprocess.run([str(p/'manager')],check=True)
print('PASS manager completion remains pending through driver teardown, cache refresh, reconnect and every failure return')
