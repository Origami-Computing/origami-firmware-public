"""Execute BLE init/deinit with each synchronization allocation failing.

NimBLE and allocators are fakes: this proves rollback/retry and call ordering,
not physical radio operation or concurrent callbacks.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/hal/hal_ble.cpp').read_text()
transfer=(ROOT/'src/services/ble_files.cpp').read_text()
def function(signature, source=source):
    start=source.index(signature+' {');end=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
helper=function('void releaseSessionResources()') if 'void releaseSessionResources()' in source else ''
defines=source[source.index('#define BLE_SVC_NOTIFY_UUID'):source.index('namespace hal')]
state=source[source.index('NimBLEServer* s_server'):source.index('// ---- BLE callbacks')]
state+=transfer[transfer.index('enum class FileState'):transfer.index('bool start()')]
helper=function('bool start()',transfer)+function('void stop()',transfer)+helper
prelude=r'''
#include "hal/hal_ble.h"
using hal::ble::FileWrite; using hal::ble::FileReceiver;
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <string>
#include <set>
std::vector<std::string> events;
int failMask=0,allocationIndex=0,nextHandle=0;
std::set<int> live;
using SemaphoreHandle_t=int*;using QueueHandle_t=int*;
int* allocate(){int bit=1<<allocationIndex++;if(failMask&bit)return nullptr;int* h=new int(++nextHandle);live.insert(*h);return h;}
int* xSemaphoreCreateMutex(){return allocate();}
int* xQueueCreate(unsigned n,unsigned bytes){assert(n==8&&bytes==258);return allocate();}
void release(int* p){assert(p&&live.erase(*p)==1);delete p;events.emplace_back("release");}
void vQueueDelete(int* p){release(p);}
void vSemaphoreDelete(int* p){release(p);}
struct File {bool open=false;explicit operator bool(){return open;}void close(){open=false;events.emplace_back("close file");}};
namespace hal {namespace bus {void lockSpi(){events.emplace_back("lock SPI");}void unlockSpi(){events.emplace_back("unlock SPI");}}}
struct Logger{void println(const char*){}template<class... T>void printf(const char*,T...){}}Serial;
struct ServerCB{};struct NotifyWriteCB{};struct FileWriteCB{FileWrite kind;explicit FileWriteCB(FileWrite value):kind(value){}};
std::vector<std::pair<std::string,FileWrite>> writes;
struct NimBLECharacteristic{
 std::string uuid;
 template<class T>void setCallbacks(T* p){assert(p);}
 void setCallbacks(FileWriteCB* p){writes.emplace_back(uuid,p->kind);}
};
struct NimBLEService{
 NimBLECharacteristic* createCharacteristic(const char* uuid,int){static NimBLECharacteristic c;c.uuid=uuid;return &c;}
 const char* getUUID(){return "notification";}
};
struct NimBLEServer{
 template<class T>void setCallbacks(T* p,bool owns){assert(p&&!owns);}
 NimBLEService* createService(const char*){static NimBLEService s;return &s;}
 void start(){events.emplace_back("server start");}
};
struct NimBLEAdvertising{void setName(const char*){}void addServiceUUID(const char*){}void start(){events.emplace_back("advertise");}};
struct NimBLEDevice{
 static void init(const char*){assert(live.size()==3);events.emplace_back("BLE init");}
 static void setMTU(int n){assert(n==256);}
 static NimBLEServer* createServer(){static NimBLEServer s;return &s;}
 static NimBLEAdvertising* getAdvertising(){static NimBLEAdvertising a;return &a;}
 static void deinit(bool release){assert(release&&live.size()==3);events.emplace_back("BLE stop");}
};
namespace NIMBLE_PROPERTY {constexpr int WRITE=1,WRITE_NR=2;}
'''
checks=r'''
void receive(FileWrite,const uint8_t*,size_t){}
void processFileTransfer(uint32_t){}
RECEIVER_TABLE
int main(){
 assert(!init("unconfigured")&&allocationIndex==0);
 ::configureFileReceiver(receiver);
 // All seven failure combinations, followed by successful retry and shutdown.
 for(int mask=1;mask<8;++mask){
  failMask=mask;allocationIndex=0;events.clear();
  assert(!init("test"));assert(!s_started&&allocationIndex==3);
  assert(live.empty()&&!s_notifMtx&&!s_fileMtx&&!s_chunkQ);
  for(const auto& event:events)assert(event=="release"); // Never start/stop NimBLE on allocation failure.
  const auto before=events.size();deinit();assert(events.size()==before);
  failMask=0;allocationIndex=0;events.clear();writes.clear();assert(init("test"));
  assert(writes.size()==3);
  assert(writes[0].first==BLE_CHR_FILE_BEGIN&&writes[0].second==FileWrite::Begin);
  assert(writes[1].first==BLE_CHR_FILE_DATA&&writes[1].second==FileWrite::Data);
  assert(writes[2].first==BLE_CHR_FILE_END&&writes[2].second==FileWrite::End);
  assert(s_started&&live.size()==3&&allocationIndex==3);
  assert((events==std::vector<std::string>{"BLE init","server start","advertise"}));
  assert(init("ignored"));assert(allocationIndex==3&&events.size()==3);
  s_file.open=true;s_notifPending=true;s_fileState=FileState::Active;
  s_filePath[0]='x';s_fileLastActivityMs=20;s_connCount=1;
  events.clear();deinit();
  assert((events==std::vector<std::string>{"BLE stop","lock SPI","close file","unlock SPI","release","release","release"}));
  assert(live.empty()&&!s_notifMtx&&!s_fileMtx&&!s_chunkQ&&!s_server&&!s_started);
  assert(!s_notifPending&&s_fileState==FileState::Idle&&!s_filePath[0]&&!s_fileLastActivityMs&&!s_connCount);
  const auto after=events.size();deinit();assert(events.size()==after);
 }
 for(int i=0;i<1000;++i){
  allocationIndex=0;events.clear();writes.clear();
  assert(init("repeat"));deinit();assert(live.empty());
 }
 puts("PASS BLE allocation failures, retry, ordered teardown and 1000 radio restarts with borrowed callbacks");
}
'''
checks=checks.replace('RECEIVER_TABLE',transfer[transfer.index('const hal::ble::FileReceiver receiver'):transfer.index('} }  // namespace')])
with tempfile.TemporaryDirectory(prefix='kami-ble-init-') as directory:
    path=Path(directory);cpp=path/'check.cpp';binary=path/'check'
    callbacks=source[source.index('static ServerCB s_serverCallbacks;'):source.index('// ---- main-loop processing')]
    cpp.write_text(prelude+defines+state+callbacks+helper+function('void configureFileReceiver(const FileReceiver& receiver)')+function('bool init(const char* device_name)')+function('void deinit()')+checks)
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
