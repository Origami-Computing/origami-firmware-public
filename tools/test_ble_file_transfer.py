"""Characterize production BLE file callbacks, SD drain and shutdown with fakes.

This preserves observed behavior, including undesirable edge cases. It does not
exercise physical BLE, FreeRTOS scheduling, SD performance or init allocation.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / 'src/hal/hal_ble.cpp').read_text()
transfer = (ROOT / 'src/services/ble_files.cpp').read_text()
state = source[source.index('NimBLEServer* s_server'):source.index('// ---- BLE callbacks')]
state += transfer[transfer.index('enum class FileState'):transfer.index('bool start()')]
callbacks = source[source.index('class FileWriteCB'):source.index('// ---- main-loop processing')]
processing = transfer[transfer.index('bool start()'):transfer.index('}  // namespace')]
processing += source[source.index('void releaseSessionResources()'):source.index('}  // namespace\n\nvoid configure')]
shutdown = source[source.index('void deinit()'):source.index('bool started()')]
prelude = r'''
#include "hal/hal_ble.h"
using hal::ble::FileWrite; using hal::ble::FileReceiver;
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <string>
#include <vector>
#include <deque>
#include <map>
#include <algorithm>
constexpr int pdTRUE=1,portMAX_DELAY=-1;
constexpr const char* FILE_WRITE="w"; // Pinned Arduino FS.h: truncating fopen mode.
constexpr uint32_t BLE_FILE_IDLE_CLOSE_MS=5000;
int pdMS_TO_TICKS(int ms){return ms;}
uint32_t clockMs=100;
uint32_t millis(){return clockMs;}
std::vector<std::string> events;
struct Mutex{bool available=true,locked=false;};
using SemaphoreHandle_t=Mutex*;
Mutex* xSemaphoreCreateMutex(){return new Mutex;}
int xSemaphoreTake(Mutex* m,int){assert(m);if(!m->available)return 0;assert(!m->locked);m->locked=true;return 1;}
void xSemaphoreGive(Mutex* m){assert(m&&m->locked);m->locked=false;}
void vSemaphoreDelete(Mutex* m){assert(m&&!m->locked);events.emplace_back("delete mutex");delete m;}
struct Queue{std::deque<std::vector<uint8_t>> chunks;};
using QueueHandle_t=Queue*;
Queue* xQueueCreate(unsigned count,unsigned size){assert(count==8&&size==258);return new Queue;}
// Definitions follow the actual FileChunk declaration.
int xQueueSend(Queue*,const void*,int);
int xQueueReceive(Queue*,void*,int);
void vQueueDelete(Queue* q){events.emplace_back("delete queue");delete q;}
bool spiLocked=false,inBleCallback=false,failOpen=false,shortWrite=false;
namespace hal {namespace bus {
void lockSpi(){assert(!inBleCallback&&!spiLocked);spiLocked=true;events.emplace_back("lock spi");}
void unlockSpi(){assert(spiLocked);spiLocked=false;events.emplace_back("unlock spi");}
}}
std::map<std::string,std::string> storage;
struct File {
 bool opened=false;std::string path;
 explicit operator bool() const{return opened;}
 size_t write(const uint8_t* bytes,size_t n){assert(opened&&spiLocked);events.emplace_back("write");if(shortWrite)n/=2;storage[path].append((const char*)bytes,n);return n;}
 void close(){assert(spiLocked);opened=false;events.emplace_back("close");}
};
struct SdFake {
 File open(const char* path,const char* mode){assert(spiLocked&&!strcmp(mode,"w"));events.emplace_back("open");if(!failOpen)storage[path].clear();return {!failOpen,path};}
} SD;
struct Logger {
 void println(const char*){}
 template<class... T> void printf(const char*,T...){}
} Serial;
struct NimBLEServer{};
struct NimBLEDevice {static void deinit(bool release){assert(release);events.emplace_back("stop BLE");}};
struct NimBLEConnInfo{};
struct NimBLEAttValue{std::vector<uint8_t> bytes;size_t length(){return bytes.size();}const uint8_t* data(){return bytes.data();}};
struct NimBLECharacteristic{NimBLEAttValue value;NimBLEAttValue getValue(){return value;}};
struct NimBLECharacteristicCallbacks{virtual void onWrite(NimBLECharacteristic*,NimBLEConnInfo&){};};
'''
checks = r'''
int xQueueSend(Queue* q,const void* chunk,int timeout){
 assert(q&&timeout==100);if(q->chunks.size()==8)return 0;
 const auto* p=(const uint8_t*)chunk;q->chunks.emplace_back(p,p+sizeof(FileChunk));return pdTRUE;
}
int xQueueReceive(Queue* q,void* chunk,int timeout){
 assert(q&&timeout==0);if(q->chunks.empty())return 0;
 memcpy(chunk,q->chunks.front().data(),sizeof(FileChunk));q->chunks.pop_front();return pdTRUE;
}
FileWriteCB beginCallback{FileWrite::Begin},dataCallback{FileWrite::Data},endCallback{FileWrite::End};

void write(NimBLECharacteristicCallbacks& callback,const std::string& data){
 NimBLECharacteristic chr;chr.value.bytes.assign(data.begin(),data.end());NimBLEConnInfo info;
 const auto before=events.size();inBleCallback=true;callback.onWrite(&chr,info);inBleCallback=false;
 assert(events.size()==before); // BLE task never performs storage I/O.
 assert(!s_fileMtx->locked);
}
void begin(const std::string& path){write(beginCallback,path);}
void data(const std::string& bytes){write(dataCallback,bytes);}
void end(){write(endCallback,"");}
void tick(uint32_t now){clockMs=now;processFileTransfer(now);assert(!spiLocked&&!s_fileMtx->locked);}
void reset(){
 s_fileReceiver=&receiver;
 if(s_started)deinit();
 s_started=true;s_notifMtx=new Mutex;s_fileMtx=new Mutex;s_chunkQ=new Queue;
 failOpen=false;shortWrite=false;clockMs=100;storage.clear();events.clear();
}
int main(){
 reset();begin("");assert(s_fileState==FileState::Idle);
 s_fileMtx->available=false;begin("/busy");tick(100);assert(s_fileState==FileState::Idle);s_fileMtx->available=true;
 begin("/first");begin("/ignored");assert(!strcmp(s_filePath,"/first"));
 data("abc");end();assert(s_fileState==FileState::OpenPending); // Early END is ignored.
 tick(100);assert(s_fileState==FileState::Active&&storage["/first"].empty()&&s_chunkQ->chunks.size()==1);
 tick(100);assert(storage["/first"]=="abc"&&s_chunkQ->chunks.empty());
 tick(5100);assert(s_fileState==FileState::Active); // Strictly greater than 5000.
 tick(5101);assert(s_fileState==FileState::Idle&&!s_file);
 begin("/first");tick(5200);data("def");end();tick(5200);
 assert(storage["/first"]=="def"&&s_fileState==FileState::Idle); // FILE_WRITE truncates on reopen.

 reset();data("orphan");tick(100);assert(s_chunkQ->chunks.size()==1);
 failOpen=true;begin("/failed");tick(100);assert(s_fileState==FileState::Idle&&s_chunkQ->chunks.size()==1);
 failOpen=false;begin("/next");tick(100);tick(100);assert(storage["/next"]=="orphan");
 data("");assert(s_chunkQ->chunks.empty());
 data(std::string(300,'x'));end();tick(100);assert(storage["/next"].size()==6+256);

 reset();begin(std::string(200,'p'));assert(strlen(s_filePath)==127&&s_filePath[127]==0);
 tick(100);for(int i=0;i<9;++i)data(std::string(1,'0'+i));
 assert(s_chunkQ->chunks.size()==8);end();tick(100);assert(storage[std::string(127,'p')]=="01234567");

 reset();begin("/short");tick(100);shortWrite=true;data("abcd");tick(100);
 assert(storage["/short"]=="ab"&&s_chunkQ->chunks.empty()&&s_fileState==FileState::Active);
 // A short write is not retried or surfaced; the activity timestamp still advances.
 assert(s_fileLastActivityMs==100);

 reset();begin("/wrap");tick(UINT32_MAX-100);tick(4899);assert(s_fileState==FileState::Active);
 tick(4900);assert(s_fileState==FileState::Idle);
 reset();begin("/late");tick(100);data("x");clockMs=101;processFileTransfer(100);
 assert(storage["/late"]=="x"&&s_fileState==FileState::Idle); // Drain timestamp newer than sampled 'now' underflows.

 reset();begin("/shutdown");tick(100);data("pending");events.clear();deinit();
 assert(storage["/shutdown"].empty()); // Shutdown closes without draining queued data.
 assert((events==std::vector<std::string>{"stop BLE","lock spi","close","unlock spi","delete queue","delete mutex","delete mutex"}));
 assert(!s_started&&!s_fileMtx&&!s_notifMtx&&!s_chunkQ&&!s_file&&s_fileState==FileState::Idle&&!s_filePath[0]&&!s_fileLastActivityMs);
 const auto n=events.size();deinit();assert(events.size()==n);
 puts("PASS BLE file protocol ordering, bounded queue, SD failures, timeout edges and shutdown contract");
}
'''
with tempfile.TemporaryDirectory(prefix='kami-ble-files-') as directory:
    path = Path(directory)
    cpp = path / 'check.cpp'
    cpp.write_text(prelude + state + callbacks + processing + shutdown + transfer[transfer.index("const hal::ble::FileReceiver receiver"):transfer.index("} }  // namespace")] + checks)
    binary = path / 'check'
    subprocess.run(['c++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I'+str(ROOT/'include'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
