"""Compile the actual early shipping HAL against register-level bus fakes."""
from pathlib import Path
import tempfile,subprocess
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory)
    def write(name,text):
        p=root/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(text)
    write('Pins.h','''#pragma once
#include <cstdint>
constexpr int SDA_PIN=15,SCL_PIN=16,LCD_RESET_PIN=45,LCD_CS_PIN=3,SD_CS=10,IO_EXPANDER_INT_B_PIN=1;
constexpr int OUTPUT=1,INPUT_PULLUP=2,LOW=0,HIGH=1;
extern int interruptLevel;
inline void pinMode(int,int){} inline void digitalWrite(int,int){}
inline int digitalRead(int){return interruptLevel;}
struct SerialT{template<class... T>void printf(const char*,T...){}void flush(){}};
extern SerialT Serial;
''')
    write('hal/hal_bus.h','''#pragma once
extern int locks;
namespace hal{namespace bus{inline void lockI2c(){++locks;}inline void unlockI2c(){--locks;}}}
''')
    write('Wire.h','''#pragma once
#include <cstdint>
#include <vector>
#include <utility>
struct WireT{
 uint8_t reg[16];std::vector<uint8_t> tx;std::vector<std::pair<int,int>> writes;
 int selected=0;bool fail=false;
 void begin(int,int){}void setTimeOut(int){}
 void beginTransmission(int){tx.clear();}void write(uint8_t b){tx.push_back(b);}
 int endTransmission(bool=true){if(fail)return 1;selected=tx[0];if(tx.size()==2){reg[selected]=tx[1];writes.push_back({selected,tx[1]});}return 0;}
 int requestFrom(uint8_t,uint8_t){return fail?0:1;}int read(){return reg[selected];}
};extern WireT Wire;
''')
    write('driver/gpio.h','#pragma once\nusing gpio_num_t=int;inline void gpio_hold_en(int){} inline void gpio_deep_sleep_hold_en(){}\n')
    write('esp_sleep.h','''#pragma once
#include <cstdint>
using esp_err_t=int;constexpr int ESP_OK=0,ESP_SLEEP_WAKEUP_ALL=0;
extern uint64_t timer;extern int ext;
inline void esp_sleep_disable_wakeup_source(int){timer=0;ext=0;}
inline int esp_sleep_enable_ext0_wakeup(int pin,int level){ext=pin+1;return 0;}
inline void esp_sleep_enable_timer_wakeup(uint64_t n){timer=n;}
struct Slept{};inline void esp_deep_sleep_start(){throw Slept{};}
''')
    write('test.cpp','''#include "Pins.h"
#include "Wire.h"
#include "esp_sleep.h"
#include "hal/shipping_power.h"
#include <cassert>
#include <cstring>
int interruptLevel=1,locks=0,ext=0;uint64_t timer=0;WireT Wire;SerialT Serial;
int main(){
 memset(Wire.reg,255,sizeof(Wire.reg));assert(hal::shipping::prepare());assert(!locks);
 assert(Wire.writes.size()==6);for(int i=0;i<3;++i){assert(Wire.writes[i].first==4+i);assert(Wire.writes[i+3].first==12+i);}
 assert(Wire.reg[4]==0 && Wire.reg[5]==0 && Wire.reg[6]==0x40);
 hal::shipping::Inputs in{};assert(hal::shipping::read(in) && in.buttons==0 && !in.usb);
 Wire.reg[1]=uint8_t(~(1<<4));Wire.reg[2]=uint8_t(~(1<<1));assert(hal::shipping::read(in)&&in.buttons==4&&in.usb);
 Wire.reg[5]=0x80;assert(!hal::shipping::outputsOff());assert(hal::shipping::prepare());
 try{hal::shipping::sleep(0,true);}catch(Slept&){}assert(ext==2 && timer==0);
 try{hal::shipping::sleep(0,false);}catch(Slept&){}assert(ext==2 && timer==1000000); // edge consumed by final read
 interruptLevel=0;try{hal::shipping::sleep(0,true);}catch(Slept&){}assert(ext==0 && timer==30000000);
 Wire.fail=true;assert(!hal::shipping::read(in));assert(!hal::shipping::prepare());assert(!locks);
}
''')
    exe=root/'test'
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(root),'-I'+str(ROOT/'include'),str(ROOT/'src/hal/shipping_power.cpp'),str(root/'test.cpp'),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS shipping HAL latch-before-direction, safe outputs, inputs, bus errors and stuck-interrupt backoff')
