"""Execute the real sleep orchestration with fake hardware: refusal never powers down."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'src/services.cpp').read_text()
a=s.index('void doBatterySleep() {');b=s.index('{',a)+1;depth=1
while depth:
    depth+=(s[b]=='{')-(s[b]=='}');b+=1
function=s[a:b]
code=r'''
#include <cassert>
#include "hal/audio_retirement.h"
hal::audio::RetirementBlock retiring;bool retireOnExit=false;
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#define ORIGAMI_DEV_SERIAL_INPUT
#define ESP_SLEEP_WAKEUP_EXT0 1
#define GPIO_NUM_1 1
#define ESP_OK 0
#define TFT_WHITE 65535
#include <ctime>
void note(const char*);
unsigned clockMs=100;unsigned millis(){return clockMs;}
namespace ui {namespace home_eyes {void sleepTransition(int){note("sleep-transition");}} namespace kami_expression {enum class Action {Wake};} namespace kami_motion {void reset(){}void trigger(kami_expression::Action,unsigned){}}}
#define SDA_PIN 1
#define SCL_PIN 2
#define pdMS_TO_TICKS(x) (x)
using esp_err_t=int;using gpio_num_t=int;
std::vector<std::string> events;void note(const char* name){events.push_back(name);}
bool onExternalPower=true,alarmScheduled=false;long fireAt=0,nowEpoch=(long)time(nullptr),armedWake=-1;
namespace core{namespace time{inline long long scheduledWakeStageSeconds(long long r){return r<=90?0:r-r/50-60;}}}
struct Logger{void println(const char*){}template<class... T>void printf(const char*,T...){}} Serial;
void delay(int){}void vTaskDelay(int){}
void esp_sleep_disable_wakeup_source(int){}int esp_sleep_enable_ext0_wakeup(int,int){return 0;}void esp_sleep_enable_timer_wakeup(uint64_t){}
void esp_light_sleep_start(){clockMs+=60000;}int esp_sleep_get_wakeup_cause(){return 0;}
struct WireType{void end(){}void begin(int,int){}} Wire;
namespace services {namespace background_network {
struct RadioPause {
 enum class Result{Acquired,PollerBusy,PlaybackBusy};static bool ready;bool held=false;
 Result acquire(){note("acquire");if(hal::audio::retirementPending())return Result::PlaybackBusy;held=ready;return ready?Result::Acquired:Result::PollerBusy;}
 void release(){if(held){note("release");held=false;}}~RadioPause(){release();}
};bool RadioPause::ready=false;
const char* resultName(RadioPause::Result){return "test";}
}}
namespace services {namespace automation{void prepareForBatterySleep(){note("lan-stop");}bool nextScheduledFire(time_t* out){*out=fireAt;return alarmScheduled;}bool wallClockSynced(){return true;}}}
namespace services {namespace vibration{void stopAll(){}}}
namespace core {
namespace sleep_timer{unsigned seconds(){return 60;}}

namespace watchdog{bool subscribed=true;void feed(){}void subscribe(){subscribed=true;}void unsubscribe(){subscribed=false;}}
}
namespace hal {
namespace io {bool readExpanderInputs(uint8_t*,uint8_t*,uint8_t*){return true;}}
namespace power {using Status=int;int read(){return 1;}bool externalPowerPresent(int){return onExternalPower;}void saveFuelWorkaroundBeforeSleep(){}}
namespace ble {void deinit(){note("ble-off");}void restoreAfterSleep(){note("ble-on");}}
namespace wifi {void stop(){note("wifi-off");}void restore(){note("wifi-on");}}
namespace display {void clearContentArea(int){}int tft(){return 0;}void setBacklight(bool on){note(on?"display-on":"display-off");}}
namespace imu {void pollWakeUp(){}int wakeUpIntPin(){return -1;}}
namespace input {void suppressUntilButtonsReleased(){note("suppress");}void resetRotaryEncoderPins(){}void beginRotaryInterrupts(){}void resyncRotaryQuadrature(){}}
namespace io {bool reinitAfterI2CRestart(){return false;}}
namespace audio {void invalidateForWake(){note("audio-invalidate");}}
}
struct NavHandler {
 static NavHandler& instance(){static NavHandler nav;return nav;}
 void shutdown(){note("nav-shutdown");}void prepareForSleep(){note("prepare-root");if(retireOnExit)retiring.acquire();}void reenterRoot(){note("root");}
 void armInputBlock(unsigned){note("block");}void invalidateChrome(){}
};
namespace sfx {constexpr int kSfxBoot=0;void play(int){note("chime");}}
bool wakeButtonHeld(){return false;}int wakeAfterCycles=0;bool sleepWakeRequested(){return wakeAfterCycles--<=0;}
unsigned s_shallowTestMs=0;unsigned s_sleepTestWakeAt=99;void notifyUserActivity(){note("activity");}
void enterDeepSleep(long long wake){armedWake=wake;note("deep-sleep");wakeAfterCycles=0;}
static constexpr uint32_t kShallowSleepMs=1800000;
bool deepSleepWakeTarget(long long* w){time_t f=0;*w=0;if(!services::automation::nextScheduledFire(&f))return true;if(core::time::scheduledWakeStageSeconds((long long)f-nowEpoch)<=0)return false;*w=f;return true;}
'''+function+r'''
int main(){
 doBatterySleep();
 assert((events==std::vector<std::string>{"prepare-root","acquire","root","block","suppress","activity"}));
 assert(core::watchdog::subscribed&&s_sleepTestWakeAt==0);
 events.clear();services::background_network::RadioPause::ready=true;s_sleepTestWakeAt=99;
 retireOnExit=true;doBatterySleep();
 assert(hal::audio::retirementPending());
 assert((events==std::vector<std::string>{"prepare-root","acquire","root","block","suppress","activity"}));
 assert(core::watchdog::subscribed);retiring.release();retireOnExit=false;
 events.clear();s_sleepTestWakeAt=99;
 doBatterySleep();
 auto at=[](const char* name){auto it=std::find(events.begin(),events.end(),name);assert(it!=events.end());return it-events.begin();};
 assert(at("acquire")<at("wifi-off")&&at("wifi-off")<at("display-off"));
 assert(at("wifi-on")<at("release")&&at("root")<at("release"));
 assert(at("release")<at("chime")&&at("chime")<at("activity"));
 assert(std::count(events.begin(),events.end(),"release")==1);
 assert(core::watchdog::subscribed&&s_sleepTestWakeAt==0);
 assert(std::count(events.begin(),events.end(),"deep-sleep")==0);
 auto ran=[](const char* name){return std::count(events.begin(),events.end(),name);};
 // A wake inside the shallow window never reaches deep sleep.
 events.clear();onExternalPower=false;alarmScheduled=false;wakeAfterCycles=5;
 doBatterySleep();
 assert(ran("deep-sleep")==0&&ran("wifi-on")==1);
 // 30 minutes of shallow sleep moves on without restoring the screen or radio first.
 events.clear();wakeAfterCycles=100;armedWake=-1;
 doBatterySleep();
 assert(ran("deep-sleep")==1&&armedWake==0&&at("nav-shutdown")<at("deep-sleep")&&at("wifi-off")<at("deep-sleep"));
 // A far-off alarm is carried into deep sleep as the timer target.
 events.clear();alarmScheduled=true;fireAt=nowEpoch+3600;wakeAfterCycles=100;
 doBatterySleep();
 assert(ran("deep-sleep")==1&&armedWake==fireAt);
 // A close alarm keeps polling, which is what can ring it.
 events.clear();fireAt=nowEpoch+60;wakeAfterCycles=40;
 doBatterySleep();
 assert(ran("deep-sleep")==0);
}
'''
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory);(root/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(ROOT/'src/hal/audio_retirement.cpp'),str(root/'test.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
print('PASS real sleep orchestration: refusal leaves radio/display/watchdog active, restores root/input; success releases network after Wi-Fi and root restore')
