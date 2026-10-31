"""Check the actual opt-in ISR probe with controlled timer/SDK boundaries."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    p = Path(directory)
    (p/'freertos').mkdir()
    (p/'driver').mkdir()
    (p/'soc').mkdir()
    (p/'soc/lcd_cam_struct.h').write_text('''#pragma once
struct LcdCam {
 struct {unsigned cam_vsync_filter_en=1;} cam_ctrl1;
 struct {unsigned cam_update=0,cam_vsync_filter_thres=4;} cam_ctrl;
};
inline LcdCam LCD_CAM;
''')
    (p/'Pins.h').write_text('#define CAMERA_VSYNC_PIN 48\n')
    (p/'driver/gpio.h').write_text('''#pragma once
using gpio_num_t=int;
constexpr int ESP_OK=0, GPIO_INTR_POSEDGE=1;
inline int addedResult=0, typeResult=0, removed=0;
inline int gpio_isr_handler_add(int,void(*)(void*),void*){return addedResult;}
inline int gpio_set_intr_type(int,int){return typeResult;}
inline int gpio_isr_handler_remove(int){++removed;return 0;}
''')
    (p/'driver/pcnt.h').write_text('''#pragma once
#include <cstdint>
using pcnt_unit_t=int;
constexpr int PCNT_UNIT_0=0,PCNT_CHANNEL_0=0,PCNT_MODE_KEEP=0,PCNT_COUNT_INC=1,PCNT_COUNT_DIS=0;
struct pcnt_config_t {int pulse_gpio_num,ctrl_gpio_num,lctrl_mode,hctrl_mode,pos_mode,neg_mode,counter_h_lim,counter_l_lim,unit,channel;};
inline int pcntResult=0,filterResult=0;inline bool counting=false;
inline int pcnt_unit_config(const pcnt_config_t* c){return pcntResult;}
inline int pcnt_counter_pause(int){counting=false;return 0;}
inline int pcnt_filter_disable(int){return filterResult;}
inline int pcnt_counter_clear(int){return 0;}
inline int pcnt_counter_resume(int){counting=true;return 0;}
inline int pcnt_get_counter_value(int,int16_t* count){*count=123;return 0;}
''')
    (p/'Arduino.h').write_text('''#pragma once
#include <string>
#include <cstdio>
#define IRAM_ATTR
struct Printer {std::string text; void printf(const char* literal){text+=literal;} template<class... T> void printf(const char* fmt,T... args) {char b[512];std::snprintf(b,sizeof b,fmt,args...);text+=b;}};
inline Printer Serial;
''')
    (p/'esp_arduino_version.h').write_text('#define ESP_ARDUINO_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))\n#define ESP_ARDUINO_VERSION 20017\n')
    (p/'esp_timer.h').write_text('#include <stdint.h>\nextern uint64_t clockUs; inline int64_t esp_timer_get_time(){return clockUs;}\n')
    (p/'freertos/FreeRTOS.h').write_text('''#pragma once
#include <cassert>
using BaseType_t=int;using portMUX_TYPE=int;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL_ISR(m) assert(!(*(m))++)
#define portEXIT_CRITICAL_ISR(m) assert((*(m))-- == 1)
#define portENTER_CRITICAL portENTER_CRITICAL_ISR
#define portEXIT_CRITICAL portEXIT_CRITICAL_ISR
''')
    (p/'freertos/task.h').write_text('')
    (p/'main.cpp').write_text('#include "'+str(ROOT/'src/hal/camera_event_diagnostics.cpp')+'"\n'+r'''
uint64_t clockUs=0; int calls=0; void* expected; BaseType_t* flag;
extern "C" void __real_ll_cam_send_event(void* c,int e,BaseType_t* f){
 assert(mutex==0&&c==expected&&f==flag);++calls;*f=e;
}
int main(){
 int camera=0,awake=0;expected=&camera;flag=&awake;
 clockUs=0xfffffff0;__wrap_ll_cam_send_event(expected,1,flag);
 clockUs+=32;__wrap_ll_cam_send_event(expected,1,flag);
 __wrap_ll_cam_send_event(expected,0,flag);
 assert(calls==3&&counters.vsync==2&&counters.dma==1&&counters.maxGapUs==32&&awake==0);
 for(int i=0;i<12;++i){clockUs+=i==11?60000:30000;__wrap_ll_cam_send_event(expected,1,flag);}
 assert(counters.gapsOver45Ms==1&&counters.maxGapUs==60000&&counters.gapCount==13);
 uint8_t b[]={255,216,255,217,255,216,255,217};
 cameraEventObserve(b,sizeof b);
 assert(Serial.text.find("recent_us=30000,30000,30000,30000,30000,30000,30000,60000")!=std::string::npos);
 for(int i=1;i<8;++i)cameraEventObserve(b,sizeof b);
 auto logged=Serial.text;
 for(int i=8;i<20;++i)cameraEventObserve(b,sizeof b);
 assert(Serial.text==logged&&suspicious==20&&frames==20);
 cameraEventBegin();assert(gpioActive&&pulseActive&&counting);
#if defined(ORIGAMI_CAMERA_VSYNC_FILTER_BYPASS)
 assert(LCD_CAM.cam_ctrl1.cam_vsync_filter_en==0&&LCD_CAM.cam_ctrl.cam_update==1);
 assert(LCD_CAM.cam_ctrl.cam_vsync_filter_thres==4);
#endif
 clockUs+=30000;gpioEdge(nullptr);clockUs+=30000;gpioEdge(nullptr);
 assert(gpioCounters.vsync==2&&gpioCounters.maxGapUs==30000);
 cameraEventFinish();assert(!gpioActive&&removed==1&&!gpioCounters.vsync&&!pulseActive&&!counting);
 gpioEdge(nullptr);assert(!gpioCounters.vsync);
 addedResult=-1;pcntResult=-1;cameraEventBegin();assert(!pulseActive&&!counting);assert(!gpioActive&&removed==1);
 addedResult=0;typeResult=-1;pcntResult=0;filterResult=-1;cameraEventBegin();assert(!pulseActive&&!counting);assert(!gpioActive&&removed==2);
 cameraEventFinish();assert(!frames&&!suspicious&&!counters.vsync&&!counters.gapCount);
}
''')
    for flags in [[], ['-DORIGAMI_CAMERA_VSYNC_FILTER_BYPASS']]:
      subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined', *flags,
                    '-DORIGAMI_CAMERA_EVENT_DIAGNOSTICS','-I'+directory,
                    str(p/'main.cpp'),'-o',str(p/'test')],check=True)
      subprocess.run([str(p/'test')],check=True)
print('PASS actual camera event probe: forwarding outside lock, timer wrap, recent interval order, bounded logs and reset under ASan/UBSan')
