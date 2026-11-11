"""Exercise the actual Error screen descriptor with fake drawing/app actions.

The Home lifecycle test separately covers actual scene exit versus uncover.
This verifies view timing/input routing; it does not simulate network errors.
"""
from pathlib import Path
import subprocess
import tempfile

ROOT=Path(__file__).resolve().parents[1]
source=(ROOT/'src/apps/agent/screen_error.cpp').read_text()
source='\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
prelude=r'''
#include "apps/agent/agent_screens.h"
#include "apps/agent/agent_config.h"
#include "core/alert_vibration.h"
#include <cassert>
#include <cstring>
#include <vector>
uint32_t clockMs=0;
uint32_t millis(){return clockMs;}
std::vector<int> events;
namespace services {namespace vibration {bool previewing(){return false;}bool previewCustom(const core::alert_vibration::Pattern&,uint32_t){events.push_back(6);return true;}}}
namespace screen_chrome_layout {const uint16_t kKamiErrorTopBarColor=0;}
namespace apps {namespace agent {
AgentState testState;
namespace flow {
AgentState& state(){return testState;}
bool wifiReady(){return false;}
void dismissNotice(){events.push_back(2);}
void openNoticeSettings(){events.push_back(3);}
void serviceNotice(uint32_t){events.push_back(7);}
void startRecording(){events.push_back(8);}
}
namespace render {
void paintError(AgentState& s){assert(!strcmp(s.errBuf,"test error"));events.push_back(4);}
void buildErrorScene(ui::scene::Scene&,int16_t,int16_t){}
}
void requestExitToMenu(){events.push_back(3);}
void prepareScreenInput(hal::input::InputEvent){events.push_back(1);}
void serviceScreenTick(uint32_t){events.push_back(5);}
}}
'''
checks=r'''
int main(){
 using namespace apps::agent;using E=hal::input::EventType;
 strcpy(testState.errBuf,"test error");clockMs=100;kScreenError.onEnter();
 assert((events==std::vector<int>{4,6}));assert(!testState.noticeInput.armed);
 events.clear();kScreenError.onTick(90000);
 assert((events==std::vector<int>{7,5})); // Persistent notice, no automatic dismissal.
 hal::input::InputEvent ev{};
 for(E event:{E::ButtonA_Release,E::ButtonA_Tap,E::ButtonB_Tap}){
  events.clear();ev.type=event;kScreenError.onInput(ev);
  assert((events==std::vector<int>{1})); // Entry gesture cannot act.
 }
 events.clear();assert(kScreenError.onChord());assert(events.empty());
 testState.noticeInput.sample(false,false);
 events.clear();ev.type=E::ButtonA_Tap;kScreenError.onInput(ev);
 assert((events==std::vector<int>{1,2}));
 for(auto kind:{notice::Kind::Offline,notice::Kind::Auth,notice::Kind::Mic}){
  testState.noticeKind=kind;events.clear();ev.type=E::ButtonB_Tap;kScreenError.onInput(ev);
  assert(events==(kind==notice::Kind::Mic?std::vector<int>{1}:std::vector<int>{1,3}));
 }
 events.clear();assert(kScreenError.onChord());assert((events==std::vector<int>{8}));
 assert(testState.recStartedByChord);
 events.clear();kScreenError.onExit();assert(!testState.errBuf[0]&&events.empty());
}
'''
with tempfile.TemporaryDirectory(prefix='kami-error-view-') as directory:
    path=Path(directory)
    (path/'Arduino.h').write_text('#pragma once\n#include <stdint.h>\n#include <stddef.h>\nuint32_t millis();\n')
    (path/'TB_TFT_eSPI.h').write_text('#pragma once\nclass TFT_eSprite;\n')
    cpp=path/'check.cpp';binary=path/'check';cpp.write_text(prelude+source+checks)
    subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(path),'-I'+str(ROOT/'include'),str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS persistent Error view, guarded fresh input, Settings/retry routing and exit cleanup')
