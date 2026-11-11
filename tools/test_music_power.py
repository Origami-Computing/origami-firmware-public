"""Compile production MusicPower with controlled storage/radio/display boundaries."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    def write(name, text):
        path = tmp / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text)
    write('services/background_network.h', '''#pragma once
#include <stdint.h>
extern bool busy,held,agentActive;extern int attempts;
namespace services {namespace agent_coordination {inline bool isSessionActive(){return agentActive;}}}
namespace services {namespace background_network {struct RadioPause {
enum class Result {Acquired,ServiceBusy};
Result acquire(uint32_t){++attempts;if(busy)return Result::ServiceBusy;held=true;return Result::Acquired;}
void release(){held=false;}
};}}
''')
    write('core/store.h', '''#pragma once
extern bool saved,writeOk;
namespace core {namespace store {
inline bool getBool(const char*,const char*,bool){return saved;}
inline bool putBool(const char*,const char*,bool v){if(!writeOk)return false;saved=v;return true;}
}}
''')
    write('hal/hal_display.h', '''#pragma once
extern bool lit;
namespace hal {namespace display {inline void setBacklight(bool v){lit=v;}}}
''')
    write('hal/hal_wifi.h', '''#pragma once
#include <cassert>
extern bool booted,portal,held;extern int stops,restores;
namespace hal {namespace wifi {
enum class RadioState {WifiOwned,Suspended,RestoringWifi};extern RadioState state;
inline bool taskBooted(){return booted;}inline bool apStarted(){return portal;}
inline RadioState radioState(){return state;}
inline void stop(){assert(held);++stops;state=RadioState::Suspended;}
inline void scheduleRestore(){assert(held);++restores;state=RadioState::RestoringWifi;}
}}
''')
    write('test.cpp', '''#include "services/music_power.h"
#include "hal/hal_wifi.h"
#include <cassert>
#include <cstdio>
bool busy=false,held=false,agentActive=false,saved=false,writeOk=true,lit=true,booted=true,portal=false;
int attempts=0,stops=0,restores=0;
namespace hal {namespace wifi {RadioState state=RadioState::WifiOwned;}}
int main(){
 services::audio::MusicPower p;p.load();p.activity(0);
 p.tick(20000,true);assert(lit&&stops==0); // default off
 assert(p.setEnabled(true));p.activity(100);
 busy=true;p.tick(200,true);assert(stops==0&&!held);
 p.tick(15100,true);assert(!lit&&p.dark()&&stops==0); // busy Wi-Fi doesn't block screen saving
 busy=false;p.tick(25100,true);assert(stops==1&&held&&p.radioOff());
 p.activity(25101);assert(lit&&!p.dark()&&held); // wake leaves local playback offline
 p.tick(40101,true);assert(!lit);
 p.tick(40102,false);assert(lit&&!held&&restores==1&&!p.radioOff()); // pause/settings/cover
 p.leave();assert(restores==1); // idempotent
 portal=true;p.tick(60000,true);assert(stops==1);portal=false;
 hal::wifi::state=hal::wifi::RadioState::WifiOwned;
 agentActive=true;p.tick(70000,true);assert(stops==1);agentActive=false;
 p.tick(90000,true);assert(stops==2);
 writeOk=false;assert(!p.setEnabled(false)&&p.enabled()&&held); // failed save preserves choice
 writeOk=true;assert(p.setEnabled(false)&&!p.enabled()&&!held&&lit&&restores==2);
 p.load();assert(!p.enabled());assert(p.setEnabled(true));p.leave();p.load();assert(p.enabled());
 hal::wifi::state=hal::wifi::RadioState::Suspended;p.tick(90000,true);assert(stops==2);
 p.leave();assert(restores==2); // don't restore another owner's radio
 hal::wifi::state=hal::wifi::RadioState::WifiOwned;p.activity(0xfffffff0u);
 p.tick(10,true);assert(lit);p.tick(15000,true);assert(!lit); // wrap-safe inactivity
 p.leave();assert(lit&&!held);
 puts("PASS Music power: persistence, idle/wake, busy retry, ownership, pause/cover/exit, wraparound");
}
''')
    subprocess.run(['c++', '-std=c++11', '-fsanitize=address,undefined',
                    '-I'+str(tmp), '-I'+str(root/'include'),
                    str(root/'src/services/music_power.cpp'), str(tmp/'test.cpp'),
                    '-o', str(tmp/'test')], check=True)
    subprocess.run([str(tmp/'test')], check=True)
