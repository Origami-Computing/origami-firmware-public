"""Execute real SW1 orchestration with fake hardware: rollback and terminal order."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'src/services.cpp').read_text()
def extract(signature):
    a=s.index(signature);b=s.index('{',a)+1;depth=1
    while depth:
        depth+=(s[b]=='{')-(s[b]=='}');b+=1
    return s[a:b]
# Battery deep sleep reuses the shutdown tail, which never returns.
function=extract('[[noreturn]] static void enterDeepSleep(')+'\n'+extract('void doPowerSwitchShutdown(int64_t wakeEpoch) {')
code=r'''
#include <cassert>
#include "hal/audio_retirement.h"
hal::audio::RetirementBlock retiring;bool retireOnExit=false;
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>
#define ESP_SLEEP_WAKEUP_ALL 0
#define GPIO_NUM_1 1
using esp_err_t=int;
std::vector<std::string> events;void note(const char* name){events.push_back(name);}
struct Logger{void println(const char*){}void flush(){}template<class... T>void printf(const char*,T...){}} Serial;
void delay(int){}
bool radioHeld=false;
namespace services {namespace background_network {
struct RadioPause {
 enum class Result{Acquired,PollerBusy,PlaybackBusy};static bool ready;
 Result acquire(){note("acquire");if(hal::audio::retirementPending())return Result::PlaybackBusy;radioHeld=ready;return ready?Result::Acquired:Result::PollerBusy;}
};bool RadioPause::ready=false;
const char* resultName(RadioPause::Result){return "test";}
}}
namespace services {namespace automation{void prepareForBatterySleep(){note("lan-stop");}}}
struct DeepSleep{};
namespace core {
namespace time{void prepareForDeepSleep(){note("clock-save");}}
namespace board{void armDeepSleepWake(int64_t wake){assert(wake==0);note("arm-wake");}}
namespace watchdog{
 bool subscribed=true;
 void feed(){assert(subscribed);}
 void subscribe(){assert(!subscribed);subscribed=true;note("subscribe");}
 void unsubscribe(){assert(subscribed);subscribed=false;note("unsubscribe");}
}}
void off(const char* name){assert(radioHeld);note(name);}
namespace hal {
namespace ble {void deinit(){off("ble-off");}}
namespace wifi {void powerDown(){off("wifi-off");}}
namespace camera {void deinit(){off("camera-off");}}
namespace audio {void deinitMic(){off("mic-off");}}
namespace sd {void end(){off("sd-off");}}
namespace input {void suppressUntilButtonsReleased(){note("suppress");}}
}
struct App{};App home;
struct NavHandler {
 App* current=&home;
 static NavHandler& instance(){static NavHandler nav;return nav;}
 App* root(){return current;}
 void shutdown(){assert(!core::watchdog::subscribed);note("shutdown");current=nullptr;if(retireOnExit)retiring.acquire();}
 void init(App* app){assert(app==&home);current=app;note("root");}
 void armInputBlock(unsigned){note("block");}
};
uint64_t s_shutdownTestWakeUs=8000000;
void notifyUserActivity(){note("activity");}
void safeOffAccessoryOutputs(){off("rails-off");}
void waitForPowerSwitchRelease(){assert(core::watchdog::subscribed);note("switch-release");}
void holdDirectPinsForDeepSleep(){off("pins-held");}
void esp_sleep_disable_wakeup_source(int){note("wake-reset");}
int esp_sleep_enable_timer_wakeup(uint64_t us){assert(us==8000000);note("timer");return 0;}
int esp_sleep_enable_ext0_wakeup(int pin,int level){assert(pin==1&&level==0);note("gpio-wake");return 0;}
void esp_deep_sleep_start(){assert(radioHeld&&!core::watchdog::subscribed);note("deep");throw DeepSleep{};}
'''+function+r'''
int main(){
 doPowerSwitchShutdown(0);
 assert((events==std::vector<std::string>{"unsubscribe","shutdown","subscribe","acquire","root","block","suppress","activity"}));
 assert(core::watchdog::subscribed&&!radioHeld&&NavHandler::instance().root()==&home);
 // A failed attempt must release the shutdown guard for a later real press.
 events.clear();services::background_network::RadioPause::ready=true;
 retireOnExit=true;doPowerSwitchShutdown(0);
 assert(hal::audio::retirementPending() && core::watchdog::subscribed && !radioHeld);
 assert((events==std::vector<std::string>{"unsubscribe","shutdown","subscribe","acquire","root","block","suppress","activity"}));
 retiring.release();retireOnExit=false;events.clear();
 bool slept=false;try{doPowerSwitchShutdown(0);}catch(const DeepSleep&){slept=true;}
 assert(slept);
 auto at=[](const char* name){auto it=std::find(events.begin(),events.end(),name);assert(it!=events.end());return it-events.begin();};
 assert(at("shutdown")<at("acquire")&&at("acquire")<at("lan-stop"));
 assert(at("acquire")<at("wifi-off")&&at("wifi-off")<at("rails-off"));
 assert(at("sd-off")<at("rails-off")&&at("rails-off")<at("switch-release"));
 assert(at("switch-release")<at("wake-reset")&&at("pins-held")<at("deep"));
 assert(at("gpio-wake")<at("deep"));
 // The power switch arms no alarm timer; the clock records its sleep start last.
 assert(at("wake-reset")<at("arm-wake")&&at("arm-wake")<at("deep"));
 assert(at("pins-held")<at("clock-save")&&at("clock-save")<at("deep"));
#ifdef ORIGAMI_DEV_SERIAL_INPUT
 assert(at("wake-reset")<at("timer")&&at("timer")<at("deep"));
#else
 assert(std::find(events.begin(),events.end(),"timer")==events.end());
#endif
 auto previous=events;doPowerSwitchShutdown(0);assert(events==previous);
}
'''
with tempfile.TemporaryDirectory() as directory:
    root=Path(directory);(root/'test.cpp').write_text(code)
    for development in (False,True):
        extra=['-DORIGAMI_DEV_SERIAL_INPUT'] if development else []
        subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(ROOT/'src/hal/audio_retirement.cpp'),*extra,str(root/'test.cpp'),'-o',str(root/'test')],check=True)
        subprocess.run([str(root/'test')],check=True)
print('PASS real shutdown: cancellation before handoff, no hardware-off on refusal, retry/root/watchdog recovery, quiescent hardware teardown, production GPIO wake and dev-only timer')
