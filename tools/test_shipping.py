"""Sanitized production shipping gate, persistence failures and USB insertion checks."""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    def write(name,text):
        path=root/name; path.parent.mkdir(parents=True,exist_ok=True); path.write_text(text)
    write('services/manufacturing.h', '#pragma once\nnamespace services { namespace manufacturing { inline bool markUnlock(){return true;} inline bool tutorialReady(){return true;} } }')
    write('Arduino.h',r'''#pragma once
#include <cstdint>
#include <cstring>
#include <string>
extern uint32_t clockMs;
inline uint32_t millis(){return clockMs;}
inline void delay(uint32_t n){clockMs+=n;}
struct Stop{int kind;};
struct SerialT {
 std::string input; size_t pos=0;
 void begin(int){} void setTxTimeoutMs(int){} void flush(){} void println(const char*){}
 template<class... T> void printf(const char*,T...){}
 int available(){return pos<input.size();} char read(){return input[pos++];}
};
extern SerialT Serial;
struct EspT{void restart(){throw Stop{2};}}; extern EspT ESP;
''')
    write('esp_system.h',r'''#pragma once
enum esp_reset_reason_t { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT, ESP_RST_SDIO, ESP_RST_USB };
extern esp_reset_reason_t resetReason;
inline esp_reset_reason_t esp_reset_reason(){return resetReason;}
''')
    write('esp_partition.h',r'''#pragma once
constexpr int ESP_PARTITION_TYPE_DATA=1,ESP_PARTITION_SUBTYPE_DATA_NVS=2;
extern bool partitionPresent;
inline const void* esp_partition_find_first(int,int,const char*){return partitionPresent?(void*)1:nullptr;}
''')
    write('nvs.h',r'''#pragma once
#include <cstdint>
using esp_err_t=int; using nvs_handle_t=int; using nvs_open_mode_t=int;
constexpr int ESP_OK=0,ESP_ERR_NVS_NOT_FOUND=1,NVS_READONLY=0,NVS_READWRITE=1;
extern bool namespaceExists,keyExists,initFail,writeFail,commitFail,readFail;
extern uint32_t stored,pending;
inline int nvs_open_from_partition(const char*,const char*,int mode,int* h){
 if(!namespaceExists && mode==NVS_READONLY)return ESP_ERR_NVS_NOT_FOUND;
 namespaceExists=true;*h=1;return 0;}
inline int nvs_get_u32(int,const char*,uint32_t* out){if(readFail)return 9;if(!keyExists)return 1;*out=stored;return 0;}
inline int nvs_set_u32(int,const char*,uint32_t value){pending=value;return writeFail?9:0;}
inline int nvs_commit(int){if(commitFail)return 9;stored=pending;keyExists=true;return 0;}
inline void nvs_close(int){}
''')
    write('nvs_flash.h','#pragma once\n#include "nvs.h"\ninline int nvs_flash_init_partition(const char*){return initFail?9:0;}\n')
    write('services/background_network.h',r'''#pragma once
extern bool radioBusy;
namespace services{namespace background_network{
struct RadioPause {enum class Result{Acquired,Busy};Result acquire(){return radioBusy?Result::Busy:Result::Acquired;}};
}}
''')
    for name,func in [('services/automation','prepareForBatterySleep'),('services/vibration','stopAll'),
                      ('hal/hal_ble','deinit'),('hal/hal_wifi','powerDown'),('hal/hal_camera','deinit'),
                      ('hal/hal_audio','deinitMic'),('hal/hal_sd','end'),('core/watchdog','unsubscribe')]:
        namespace=name.replace('/hal_','/').split('/')
        write(name+'.h','#pragma once\nextern int cleanup;\nnamespace '+namespace[0]+'{namespace '+namespace[1]+'{inline void '+func+'(){++cleanup;}}}\n')
    write('test.cpp',r'''
#include "Arduino.h"
#include "services/shipping.h"
#include "esp_system.h"
#include "hal/shipping_power.h"
#include <cassert>
#include <vector>
#include <utility>
uint32_t clockMs=0,stored=0,pending=0;
esp_reset_reason_t resetReason=ESP_RST_SW;
bool partitionPresent=true,namespaceExists=false,keyExists=false,initFail=false,writeFail=false,commitFail=false,readFail=false,radioBusy=false;
bool usb=false,valid=true,prepared=true;int cleanup=0,sleeps=0;uint32_t retry=0;
SerialT Serial;EspT ESP;
std::vector<std::pair<uint32_t,uint8_t>> samples;
std::vector<std::pair<uint32_t,bool>> usbSamples;
namespace hal{namespace shipping{
bool prepare(){return prepared;} bool outputsOff(){return prepared;}
bool read(Inputs& out){out.usb=usb;for(auto p:usbSamples)if(clockMs>=p.first)out.usb=p.second;out.buttons=0;for(auto p:samples)if(clockMs>=p.first)out.buttons=p.second;return valid;}
void sleep(uint32_t seconds,bool){retry=seconds;++sleeps;throw Stop{1};}
}}
using services::shipping::State;
void reset(bool locked=true){resetReason=ESP_RST_SW;clockMs=0;partitionPresent=true;namespaceExists=keyExists=locked;
 stored=0x53485001;initFail=writeFail=commitFail=readFail=radioBusy=false;
 valid=prepared=true;usb=false;cleanup=sleeps=0;retry=0;Serial={};samples.clear();usbSamples.clear();}
void expectSleep(){try{services::shipping::bootGate();assert(false);}catch(Stop s){assert(s.kind==1);}}
void sequence(){uint32_t at=100;for(uint8_t b:{1,2,4}){samples.push_back({at,b});at+=1000;samples.push_back({at,0});at+=250;}}
int main(){
 reset(false);assert(services::shipping::state()==State::Normal);assert(!services::shipping::bootGate());
 partitionPresent=false;assert(services::shipping::state()==State::Unsupported);assert(!services::shipping::enter());
 reset();initFail=true;assert(services::shipping::state()==State::Fault);expectSleep();
 reset();keyExists=false;assert(services::shipping::state()==State::Fault);expectSleep();
 reset();stored=17;assert(services::shipping::state()==State::Fault);
 reset();readFail=true;assert(services::shipping::state()==State::Fault);expectSleep();
 reset();prepared=false;expectSleep();
 reset();expectSleep();assert(stored==0x53485002); // removal durably arms insertion
 clockMs=0;usb=true;assert(services::shipping::bootGate());assert(stored==0x53485000);
 reset();usb=true;expectSleep();assert(clockMs==15000 && stored==0x53485001);
 clockMs=0;expectSleep();assert(stored==0x53485001); // attached reset never unlocks
 // Power can disappear while the durable phase still waits for removal.
 reset();usb=true;resetReason=ESP_RST_POWERON;assert(services::shipping::bootGate());assert(clockMs==150 && stored==0x53485000);
 reset();resetReason=ESP_RST_POWERON;expectSleep();assert(stored==0x53485002); // no USB: stay shipping
 reset();usb=true;resetReason=ESP_RST_POWERON;usbSamples={{100,false}};expectSleep();assert(stored==0x53485002); // unstable power-good cannot unlock
 reset();usb=true;resetReason=ESP_RST_POWERON;commitFail=true;expectSleep();assert(stored==0x53485001);
 reset();usb=true;resetReason=ESP_RST_POWERON;writeFail=true;expectSleep();assert(stored==0x53485001);
 reset();usb=true;resetReason=ESP_RST_POWERON;valid=false;expectSleep();assert(stored==0x53485001);
 reset();usb=true;resetReason=ESP_RST_POWERON;prepared=false;expectSleep();assert(stored==0x53485001);
 reset();usb=true;resetReason=ESP_RST_POWERON;stored=17;expectSleep();assert(stored==17);
 for(auto reason:{ESP_RST_UNKNOWN,ESP_RST_EXT,ESP_RST_SW,ESP_RST_PANIC,ESP_RST_INT_WDT,ESP_RST_TASK_WDT,ESP_RST_WDT,ESP_RST_DEEPSLEEP,ESP_RST_BROWNOUT,ESP_RST_SDIO,ESP_RST_USB}) {
  reset();usb=true;resetReason=reason;expectSleep();assert(stored==0x53485001);
  reset();usb=true;stored=0x53485002;resetReason=reason;assert(services::shipping::bootGate());assert(stored==0x53485000); // existing saved-removal path
 }
 reset();sequence();expectSleep();assert(stored==0x53485002); // old gesture cannot unlock
 reset();stored=0x53485002;samples={{0,7}};expectSleep();assert(stored==0x53485002);
 reset();usb=true;usbSamples={{50,false},{100,true}};expectSleep();assert(stored==0x53485001); // removal bounce
 reset();stored=0x53485002;usb=true;usbSamples={{100,false}};expectSleep();assert(stored==0x53485002); // insertion bounce
 reset();stored=0x53485002;usb=true;assert(services::shipping::bootGate());assert(clockMs==150);
 reset();stored=0x53485002;usb=true;commitFail=true;expectSleep();assert(stored==0x53485002);
 reset();commitFail=true;expectSleep();assert(stored==0x53485001 && retry==30); // removal commit failure stays locked
 reset();valid=false;expectSleep();assert(stored==0x53485001);
 reset();clockMs=UINT32_MAX-75;stored=0x53485002;usb=true;assert(services::shipping::bootGate());assert(stored==0x53485000); // debounce across clock wrap
 reset();stored=17;usb=true;expectSleep();assert(stored==17); // corrupt state cannot auto-unlock
 reset();usb=true;usbSamples={{14950,false}};expectSleep();assert(stored==0x53485001 && retry==1); // incomplete debounce
 reset();usb=true;Serial.input="shipping recover CONFIRM\n";assert(services::shipping::bootGate());assert(services::shipping::state()==State::Normal);
 reset();Serial.input="shipping recover CONFIRM\n";expectSleep(); // USB required
 reset(false);writeFail=true;assert(!services::shipping::enter());assert(cleanup==0);
 reset(false);commitFail=true;assert(!services::shipping::enter());assert(cleanup==0);
 reset(false);radioBusy=true;assert(!services::shipping::enter());assert(cleanup==0 && !keyExists);
 reset(false);try{services::shipping::enter();assert(false);}catch(Stop s){assert(s.kind==2);}
 assert(services::shipping::state()==State::Locked && cleanup>0);

}
''')
    exe=root/'shipping-test'
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(root),'-I'+str(ROOT/'include'),str(ROOT/'src/services/shipping.cpp'),str(root/'test.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS production shipping gate: USB phases, power-on activation, non-power resets, debounce, ignored buttons, persistence failures, recovery and shutdown')
