"""Execute the production BLE notification callback/inbox/parser with host fakes.

Pinned ArduinoJson is real; BLE delivery, mutex, time and scheduling are faked.
This does not verify physical BLE writes, radio coexistence or file transfer.
"""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
headers=ROOT/'.pio/libdeps/esp32-s3-menu-n16r8/ArduinoJson/src'
assert (headers/'ArduinoJson.h').exists(), 'Run the N16R8 build first'
source=(ROOT/'src/hal/hal_ble.cpp').read_text()
callback=source[source.index('class NotifyWriteCB'):source.index('class FileWriteCB')]
processing=source[source.index('void processNotification('):source.index('// Caller must have stopped')]
interpretation=(ROOT/'src/services/automation_ble.cpp').read_text()
interpretation=interpretation[interpretation.index('namespace services'): ]
service=source[source.index('void service(uint32_t'):source.index('void prepareForSleep()')]
prelude=r'''
#include <ArduinoJson.h>
#include "services/automation.h"
#include "hal/hal_ble.h"
using hal::ble::NotificationHandler;
#include <cassert>
#include <cstdio>
#include <cstring>
#include <strings.h>
#include <string>
#include <vector>
#include <cstdint>
static bool available=true,locked=false;
using SemaphoreHandle_t=int;constexpr int pdTRUE=1;
int s_notifMtx=1;bool s_notifPending=false;char s_notifJson[512];
int pdMS_TO_TICKS(int x){return x;}
int xSemaphoreTake(int mutex,int){assert(mutex==1);if(!available)return 0;assert(!locked);locked=true;return 1;}
void xSemaphoreGive(int mutex){assert(mutex==1&&locked);locked=false;}
struct Logger {std::vector<std::string> messages;void println(const char* s){messages.push_back(s);}template<class... T>void printf(const char* fmt,T...args){char text[1024];snprintf(text,sizeof(text),fmt,args...);messages.push_back(text);}} Serial;
struct NimBLEConnInfo{};
struct NimBLEAttValue{std::vector<uint8_t> bytes;size_t length(){return bytes.size();}const uint8_t* data(){return bytes.data();}};
struct NimBLECharacteristic{NimBLEAttValue value;NimBLEAttValue getValue(){return value;}};
struct NimBLECharacteristicCallbacks{virtual void onWrite(NimBLECharacteristic*,NimBLEConnInfo&){};};
static bool s_started=false,fileProcessed=false;
void processFileTransfer(uint32_t){fileProcessed=true;}
const hal::ble::FileReceiver fileReceiver{nullptr,nullptr,nullptr,processFileTransfer};
const hal::ble::FileReceiver* s_fileReceiver=&fileReceiver;
static unsigned calls=0;static bool accepted=true;static time_t clockSeconds=1700000000,scheduledTime;
static services::automation::Type scheduledType;static std::string scheduledMessage;
namespace services {namespace automation {
bool schedule(Type t,time_t fire,const char* message){assert(!locked&&!s_notifPending&&!fileProcessed);++calls;scheduledType=t;scheduledTime=fire;scheduledMessage=message;return accepted;}
}}
time_t fakeTime(time_t*){return clockSeconds;}
#define time fakeTime
'''
checks=r'''
#undef time
void drainNotification(){processNotification(services::automation::ingestBleNotification);}
NotifyWriteCB callback;
void write(const std::string& json){
 NimBLECharacteristic characteristic;characteristic.value.bytes.assign(json.begin(),json.end());NimBLEConnInfo info;
 static_cast<NimBLECharacteristicCallbacks&>(callback).onWrite(&characteristic,info);
 assert(!locked);
}
void process(const std::string& json){write(json);drainNotification();assert(!s_notifPending&&!locked);}
int main(){
 drainNotification();assert(!calls);
 write("");assert(!s_notifPending);
 available=false;write("{}");assert(!s_notifPending);available=true;
 write("{}");processNotification(nullptr);assert(s_notifPending&&!calls);write("{\"type\":\"red\"}");assert(!strcmp(s_notifJson,"{}"));
 available=false;drainNotification();assert(s_notifPending&&!calls);available=true;
 drainNotification();assert(calls==1&&!s_notifPending&&scheduledType==services::automation::Type::Yellow);
 assert(scheduledTime==clockSeconds+3&&scheduledMessage=="BLE Notification");
 auto previous=calls;process("{");assert(calls==previous&&Serial.messages.back()=="[ble] bad notification JSON");
 process("{\"type\":\"rEd\",\"time\":1800000000,\"message\":\"test\"}");
 assert(scheduledType==services::automation::Type::Red&&scheduledTime==1800000000&&scheduledMessage=="test");
 process("{\"type\":\"GREEN\",\"time\":1000000000,\"message\":\"\"}");
 assert(scheduledType==services::automation::Type::Green&&scheduledTime==1000000000&&scheduledMessage.empty());
 process("{\"type\":\"unknown\",\"time\":999999999}");
 assert(scheduledType==services::automation::Type::Yellow&&scheduledTime==clockSeconds+3);
 process("{\"time\":-1}");assert(scheduledTime==clockSeconds+3);
 accepted=false;previous=calls;process("{}");assert(calls==previous+1&&Serial.messages.back()=="[ble] notification schedule failed (queue full?)");
 accepted=true;clockSeconds=0;process("{}");assert(scheduledTime==3); // Scheduler retains clock admission.
 write(std::string(600,'x'));assert(s_notifPending&&strlen(s_notifJson)==511&&s_notifJson[511]==0);
 previous=calls;drainNotification();assert(calls==previous&&!s_notifPending);
 process(std::string("{}\0ignored",10));assert(calls==previous+1); // Existing C-string interpretation.
 write("{}");service(0,services::automation::ingestBleNotification);assert(s_notifPending&&!fileProcessed);
 s_started=true;service(0,nullptr);assert(s_notifPending&&fileProcessed);
 fileProcessed=false;service(0,services::automation::ingestBleNotification);assert(!s_notifPending&&fileProcessed);
 puts("PASS BLE mailbox contention/drop/truncation and real JSON defaults, color/time rules, scheduler failure");
}
'''
with tempfile.TemporaryDirectory(prefix='kami-ble-notification-') as directory:
    root=Path(directory); cpp=root/'test.cpp';binary=root/'test'
    cpp.write_text(prelude+callback+processing+interpretation+service+checks)
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(ROOT/'include'),'-I'+str(headers),str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
