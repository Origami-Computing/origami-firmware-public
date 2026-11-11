"""Exercise production API-key persistence/revision with synthetic secrets."""
from pathlib import Path
import argparse,subprocess,tempfile
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--concurrency',action='store_true')
parser.add_argument('--negative-control',action='store_true')
parser.add_argument('--response-race',action='store_true')
parser.add_argument('--production-policy',action='store_true')
parser.add_argument('--staging-policy',action='store_true')
args=parser.parse_args()
assert not args.negative_control or args.concurrency, 'negative control requires --concurrency'
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    def write(name,text):
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    write('freertos/FreeRTOS.h','#pragma once\n#define portMAX_DELAY 0xffffffffu\n')
    write('freertos/semphr.h','''#pragma once
#include <mutex>
#include <atomic>
inline std::atomic<unsigned>& waitingWriters(){static std::atomic<unsigned> n{0};return n;}
using StaticSemaphore_t=std::mutex;
using SemaphoreHandle_t=std::mutex*;
inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t* m){return m;}
inline bool xSemaphoreTake(SemaphoreHandle_t m,unsigned){if(!m->try_lock()){++waitingWriters();m->lock();--waitingWriters();}return true;}
inline bool xSemaphoreGive(SemaphoreHandle_t m){m->unlock();return true;}
''')
    write('freertos/portmacro.h','''#pragma once
#include <mutex>
#include <cassert>
using portMUX_TYPE=std::mutex;
inline unsigned& backendLockDepth(){static thread_local unsigned depth=0;return depth;}
inline void lockBackend(portMUX_TYPE* m){m->lock();++backendLockDepth();}
inline void unlockBackend(portMUX_TYPE* m){--backendLockDepth();m->unlock();}
#define portMUX_INITIALIZER_UNLOCKED {}
#define portENTER_CRITICAL(m) lockBackend(m)
#define portEXIT_CRITICAL(m) unlockBackend(m)
''')
    write('Arduino.h','''#pragma once
#include <string>
#include <cstdio>
#include "freertos/portmacro.h"
using String=std::string;
struct Log { void println(const char*){assert(!backendLockDepth());} template<class... T> void printf(const char*,T...){assert(!backendLockDepth());} }; extern Log Serial;
''')
    write('Preferences.h','''#pragma once
#include "Arduino.h"
#include <map>
extern std::map<std::string,std::string> store;extern std::string failKey;
inline void (*&beforeWrite())(const char*) {static void (*f)(const char*)=nullptr;return f;}
struct Preferences {
bool begin(const char*,bool){assert(!backendLockDepth());return failKey!="begin";}void end(){}
bool isKey(const char*k){return store.count(k);}
String getString(const char*k,String d){return isKey(k)?store[k]:d;}
size_t putString(const char*k,const char*v){if(beforeWrite())beforeWrite()(k);if(failKey==k)return 0;store[k]=v;return store[k].size();}
bool remove(const char*k){if(failKey==k)return false;return store.erase(k);}
bool clear(){store.clear();return true;}
bool getBool(const char*k,bool d){return isKey(k)?store[k]=="1":d;}
size_t putBool(const char*k,bool v){if(failKey==k)return 0;store[k]=v?"1":"0";return 1;}
};
''')
    write('core/menu_config.h','''#pragma once
#define WIFI_SSID ""
#define WIFI_PASS ""
#define BACKEND_URL "http://test.invalid"
#define KAMI_PRODUCTION_API_URL "http://prod.invalid"
#define KAMI_STAGING_API_URL "http://staging.invalid"
#if ORIGAMI_STAGING_BUILD
#define BACKEND_URL_PROD KAMI_STAGING_API_URL
#else
#define BACKEND_URL_PROD KAMI_PRODUCTION_API_URL
#endif
#define AUTH_TOKEN "initial-test-key"
''')
    write('main.cpp','''#include "core/secrets.h"
#include "Preferences.h"
#include <cassert>
#include <cstring>
Log Serial;std::map<std::string,std::string> store;std::string failKey;
int main(){using namespace core::secrets;init();
assert(!setApiKey(nullptr)&&apiKeyRevision()==0);
assert(setApiKey("initial-test-key")&&apiKeyRevision()==0);
assert(setDeviceToken("synthetic-token"));
assert(setApiKey("changed-test-key")&&apiKeyRevision()==1);
assert(store["api_key"]=="changed-test-key"&&!store.count("dev_token"));
assert(!strcmp(backendConfig().apiKey,"changed-test-key"));
assert(setDeviceToken("new-synthetic-token"));failKey="dev_token";
assert(!setApiKey("blocked-key")&&apiKeyRevision()==1);
assert(!strcmp(backendConfig().apiKey,"changed-test-key"));
failKey="api_key";assert(!setApiKey("failed-key")&&apiKeyRevision()==1);
assert(!strcmp(backendConfig().apiKey,"changed-test-key"));
failKey.clear();assert(setApiKey("recovered-key")&&apiKeyRevision()==2);
const auto before=backendConfig();assert(!strcmp(before.apiKey,"recovered-key"));
assert(setDeviceToken("synthetic-token"));failKey="dev_token";
assert(!setDevMode(true)&&!isDevMode());
assert(!strcmp(backendConfig().url,before.url));
failKey="begin";assert(!setDevMode(false));
failKey="server_mode";assert(!setDevMode(true)&&!isDevMode());
failKey.clear();assert(setDevMode(true)&&isDevMode());
assert(!store.count("dev_token")&&!store.count("dev_mode"));
assert(!strcmp(backendConfig().url,"http://test.invalid"));
assert(setDeviceToken("next-token"));failKey="backend_dev";
assert(!setBackendUrl("http://failed.invalid"));
assert(!store.count("dev_token")); // Token clear precedes the failed URL write.
assert(!strcmp(backendConfig().url,"http://test.invalid"));
failKey.clear();assert(setBackendUrl("http://saved.invalid"));
assert(setDevMode(false));assert(!strcmp(backendConfig().url,"http://prod.invalid"));
assert(setDevMode(true));assert(!strcmp(backendConfig().url,"http://saved.invalid"));
const auto saved=backendConfig();const auto revision=apiKeyRevision();
assert(factoryReset());const auto reset=backendConfig();
assert(!strcmp(saved.url,"http://saved.invalid")&&!strcmp(saved.apiKey,"recovered-key"));
assert(!strcmp(reset.url,"http://test.invalid")&&!strcmp(reset.apiKey,"initial-test-key"));
assert(apiKeyRevision()==revision); // Reset retains its separate lifecycle contract.
// Responses belong to the request's backend and token generation.
auto first=backendRequest();assert(!hasDeviceToken());
assert(setDeviceTokenFor(first,"registered"));assert(hasDeviceToken());
assert(!setDeviceTokenFor(first,"late-registration"));
auto registered=backendRequest();assert(!strcmp(registered.token,"registered"));
assert(setBackendUrl("http://replacement.invalid"));
assert(!setDeviceTokenFor(registered,"old-response"));
auto replacement=backendRequest();assert(setDeviceTokenFor(replacement,"new-token"));
assert(!setDeviceTokenFor(registered,""));assert(!strcmp(backendRequest().token,"new-token"));
auto current=backendRequest();failKey="dev_token";
assert(!setDeviceTokenFor(current,""));assert(!strcmp(backendRequest().token,"new-token"));
failKey.clear();assert(setDeviceTokenFor(current,""));assert(!hasDeviceToken());
auto unchanged=backendRequest();assert(setApiKey(unchanged.backend.apiKey));
assert(setDeviceTokenFor(unchanged,"same-config-token"));
auto beforeReset=backendRequest();assert(factoryReset());
assert(!setDeviceTokenFor(beforeReset,"stale-reset-token"));

}
''')
    if args.production_policy:
        assert not (args.concurrency or args.response_race or args.negative_control)
        write('main.cpp',r'''
#include "core/secrets.h"
#include "Preferences.h"
#include <cassert>
#include <cstring>
Log Serial;std::map<std::string,std::string> store;std::string failKey;
int main(){using namespace core::secrets;
 store["server_mode"]="Staging";store["dev_token"]="staging-token";init();
 assert(serverMode()==ServerMode::Prod&&!hasDeviceToken());
 assert(!setServerMode(ServerMode::Staging)&&!setServerMode(ServerMode::Dev));
 for(bool savedDev:{false,true})for(bool openFails:{false,true}){
  store.clear();store["dev_mode"]=savedDev?"1":"0";
  store["backend_dev"]="https://staging.invalid";store["dev_token"]="saved-token";
  failKey=openFails?"begin":"";init();
  assert(!isDevMode()&&!strcmp(backendConfig().url,"http://prod.invalid"));
  assert(!strcmp(backendRequest().backend.url,"http://prod.invalid"));
  if(savedDev||openFails)assert(!hasDeviceToken());
  failKey.clear();assert(setDeviceToken("production-token"));
  const auto before=backendRequest();const auto saved=store;
  for(const char* url:{"https://staging.invalid","http://localhost:3000","http://prod.invalid.evil","http://prod.invalid/",""}){
   assert(!setBackendUrl(url));assert(!setDevMode(true));
   assert(store==saved);
   assert(backendRequest().revision==before.revision);
   assert(!strcmp(backendRequest().token,"production-token"));
  }
  assert(setBackendUrl("http://prod.invalid"));
  assert(!strcmp(backendRequest().token,"production-token"));
  assert(setDevMode(false));
  init(); // A migrated production token survives reboot.
  assert(!isDevMode()&&!strcmp(backendRequest().token,"production-token"));
  assert(factoryReset());
  assert(!isDevMode()&&!strcmp(backendConfig().url,"http://prod.invalid"));
  assert(!hasDeviceToken());
 }
 for(const char* failure:{"dev_token","server_mode"}){
  store.clear();store["dev_mode"]="1";store["dev_token"]="staging-token";
  failKey=failure;init();assert(!isDevMode()&&!hasDeviceToken());
  assert(!strcmp(backendConfig().url,"http://prod.invalid"));
  assert(store["dev_mode"]=="1");
  failKey.clear();init();assert(!hasDeviceToken()&&store["server_mode"]=="Prod");
  assert(setDeviceToken("production-token"));init();
  assert(!strcmp(backendRequest().token,"production-token"));
 }
}
''')
    if args.staging_policy:
        write('main.cpp',r'''#include "core/secrets.h"
#include "Preferences.h"
#include <cassert>
#include <cstring>
Log Serial;std::map<std::string,std::string> store;std::string failKey;
int main(){using namespace core::secrets;
 init();assert(serverMode()==ServerMode::Staging);
 assert(!strcmp(backendConfig().url,"http://staging.invalid"));
 for(auto mode:{ServerMode::Prod,ServerMode::Staging,ServerMode::Dev}){
  assert(setDeviceToken("prior-environment-token"));auto old=backendRequest();
  assert(setServerMode(mode));assert(!hasDeviceToken());
  assert(!setDeviceTokenFor(old,"stale-response"));
  const char* expected=mode==ServerMode::Prod?"http://prod.invalid":mode==ServerMode::Staging?"http://staging.invalid":"http://test.invalid";
  assert(serverMode()==mode&&!strcmp(backendConfig().url,expected));
  assert(setDeviceToken("current-token"));init();
  assert(serverMode()==mode&&!strcmp(backendConfig().url,expected));assert(hasDeviceToken());
  assert(setServerMode(mode)&&hasDeviceToken());
 }
 assert(!setServerMode(static_cast<ServerMode>(99)));
 assert(setServerMode(ServerMode::Staging));assert(setDeviceToken("current"));
 for(const char* failure:{"dev_token","server_mode"}){
  failKey=failure;assert(!setServerMode(ServerMode::Prod));
  assert(serverMode()==ServerMode::Staging&&!strcmp(backendConfig().url,"http://staging.invalid"));
  failKey.clear();
 }
 // Legacy developer choice migrates; otherwise staging defaults to its hosted API.
 store.clear();store["dev_mode"]="1";init();assert(serverMode()==ServerMode::Dev);
 store.clear();store["dev_mode"]="0";init();assert(serverMode()==ServerMode::Staging);
}''')
    if args.concurrency:
        # Actual setters and snapshot reader. The negative control synchronizes
        # a setter after borrowing, deterministically exposing unstable lifetime.
        read='const auto request=backendRequest(); assert(!request.token[0]); const auto config=request.backend; const char* key=config.apiKey;'
        validate='''assert(strlen(key)==191); for(int j=1;j<191;++j)assert(key[j]==key[0]);
  assert(!strcmp(config.url,"http://prod.invalid")||!strcmp(config.url,"http://test.invalid")||
         !strcmp(config.url,"http://one.invalid/base")||!strcmp(config.url,"https://two.invalid/"));'''
        if args.negative_control:
            read='const char* key=apiKey();'
        concurrency_main='''#include "core/secrets.h"
#include "Preferences.h"
#include <cassert>
#include <cstring>
#include <atomic>
#include <thread>
Log Serial;std::map<std::string,std::string> store;std::string failKey;
int main(){using namespace core::secrets;init();
 std::string a(191,'a'),b(191,'b');assert(setApiKey(a.c_str()));
 const auto retained=backendConfig();assert(setApiKey(b.c_str()));
 assert(!strcmp(retained.apiKey,a.c_str()));
 std::atomic<bool> start{false};
 std::thread writer([&]{while(!start.load()){}for(int i=0;i<20000;++i){
  assert(setApiKey((i%2?a:b).c_str()));
  assert(setBackendUrl(i%2?"http://one.invalid/base":"https://two.invalid/"));
  assert(setDevMode(i%2));
 }});
 start=true;
 for(int i=0;i<100000;++i){
'''+read+validate+'''
 }
 writer.join();
}
'''
        if args.negative_control:
            start=concurrency_main.index(' std::atomic<bool> start')
            concurrency_main=concurrency_main[:start]+'''
 const char* borrowed=apiKey();
 std::thread writer([&]{assert(setApiKey(a.c_str()));});writer.join();
 assert(strcmp(borrowed,b.c_str())!=0);
 assert(!strcmp(retained.apiKey,a.c_str()));
}
'''
        write('main.cpp',concurrency_main)
    if args.response_race:
        write('main.cpp',r'''#include "core/secrets.h"
#include "Preferences.h"
#include "freertos/semphr.h"
#include <thread>
#include <atomic>
#include <cstring>
Log Serial;std::map<std::string,std::string> store;std::string failKey;
std::atomic<bool> entered{false},released{false};
const char* pauseKey;
void pauseWrite(const char* key){if(strcmp(key,pauseKey))return;entered=true;while(!released.load())std::this_thread::yield();}
void wait(std::atomic<bool>& value){while(!value.load())std::this_thread::yield();}
int main(){using namespace core::secrets;init();assert(setDevMode(true));
 for(bool responseFirst:{true,false}) {
  assert(setBackendUrl("http://original.invalid"));
  auto request=backendRequest();assert(!hasDeviceToken());
  entered=false;released=false;pauseKey=responseFirst?"dev_token":"backend_dev";beforeWrite()=pauseWrite;
  bool accepted=false;std::thread first([&]{if(responseFirst)accepted=setDeviceTokenFor(request,"late-token");else assert(setBackendUrl("http://replacement.invalid"));});
  wait(entered);
  std::thread second([&]{if(responseFirst)assert(setBackendUrl("http://replacement.invalid"));else accepted=setDeviceTokenFor(request,"late-token");});
  while(!waitingWriters().load())std::this_thread::yield();
  released=true;first.join();second.join();beforeWrite()=nullptr;
  assert(accepted==responseFirst);
  const auto current=backendRequest();
  assert(!strcmp(current.backend.url,"http://replacement.invalid"));
  assert(!current.token[0]&&!store.count("dev_token"));
 }
 puts("PASS both persistence overlap orders: response before setter and setter before response");
}
''')
    secrets_source=ROOT/'src/core/secrets.cpp'
    if args.negative_control:
        # Keep the retired borrowed API only in a pinned historical control.
        secrets_source=root/'legacy_secrets.cpp'
        secrets_source.write_text(subprocess.check_output(['git','show','cae6c0fe:src/core/secrets.cpp'],cwd=ROOT,text=True))
        main=root/'main.cpp'
        main.write_text('namespace core { namespace secrets { const char* apiKey(); } }\n'+main.read_text())
    binary=root/'test'
    for flags in ([[]] if args.concurrency or args.response_race else [[],['-DMENU_APP']]):
        if args.production_policy: flags=flags+['-DORIGAMI_PRODUCTION_BUILD=1']
        if args.staging_policy: flags=flags+['-DORIGAMI_STAGING_BUILD=1']
        subprocess.run(['c++','-std=c++11','-pthread','-g','-fsanitize='+('thread' if args.concurrency or args.response_race else 'address,undefined'),*flags,'-I'+str(root),'-I'+str(ROOT/'include'),str(secrets_source),str(root/'main.cpp'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True,timeout=30)
        if args.negative_control:
            print('PASS negative control: borrowed credential changes after a concurrent setter; owned copy remains stable')
print('PASS owned configuration lifetime and concurrent setters/readers' if args.concurrency and not args.negative_control else 'PASS secret contract checks')
