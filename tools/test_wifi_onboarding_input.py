"""Compile the real Wi-Fi input handler with fake navigation/session scheduling."""
from pathlib import Path
import os,shlex,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'src/apps/wifi_setup_app.cpp').read_text()
handler=s[s.index('void onInput(hal::input::InputEvent ev) {'):s.index('\nbool tutorialActive()')]
code=r'''
#include <cassert>
#include <initializer_list>
namespace hal {namespace input {enum class EventType {ButtonA_Tap,ButtonB_Tap,ButtonC_Tap,Scroll};struct InputEvent {EventType type;int delta=0;};}}
namespace onboarding {enum class WifiResult {None,Joined,Back};}
enum class State {PREPARING,LISTENING,SCANNING,CONNECTING,SUCCESS,ERROR};
State s_state;bool s_tutorialMode=true,s_audioMode=true,s_restartPending=false,s_restartAudio=false;unsigned s_errorSelection=0;
const char *s_errorTitle="",*s_errorDetail="";auto s_tutorialResult=onboarding::WifiResult::None;
int pops=0;struct NavHandler{static NavHandler& instance(){static NavHandler n;return n;}void pop(){pops++;}};
void drawStatusScreen(const char*,const char*,const char*,bool){}void prepareWorkflow(){}void enterView(){}
namespace ui {namespace nav {bool isBackTap(hal::input::InputEvent e){return e.type==hal::input::EventType::ButtonA_Tap;}bool isEnterTap(hal::input::InputEvent e){return e.type==hal::input::EventType::ButtonC_Tap;}}}
'''+handler+r'''
int main(){using E=hal::input::EventType;
for(bool tutorial:{false,true})for(bool audio:{false,true})for(auto state:{State::PREPARING,State::LISTENING,State::SCANNING}){
 s_tutorialMode=tutorial;s_audioMode=audio;s_state=state;s_restartPending=false;
 onInput({E::ButtonC_Tap});assert(s_restartPending&&s_restartAudio==!audio);onInput({E::ButtonC_Tap});assert(s_restartAudio==!audio);
}
s_tutorialMode=true;s_state=State::ERROR;s_audioMode=true;
for(auto key:{E::ButtonA_Tap,E::ButtonC_Tap}){s_restartPending=false;onInput({key});assert(s_restartPending&&s_restartAudio==(key==E::ButtonA_Tap));}
s_audioMode=false;s_restartPending=false;onInput({E::Scroll,1});onInput({E::ButtonC_Tap});assert(s_restartPending&&s_restartAudio);
s_restartPending=false;onInput({E::ButtonA_Tap});assert(s_restartPending&&!s_restartAudio);
s_restartPending=false;s_state=State::SUCCESS;pops=0;onInput({E::ButtonC_Tap});assert(!s_restartPending&&pops==0);onInput({E::ButtonB_Tap});assert(pops==1&&s_tutorialResult==onboarding::WifiResult::Joined);
}
'''
with tempfile.TemporaryDirectory(dir=ROOT/'.cache') as d:
 p=Path(d);(p/'test.cpp').write_text(code)
 subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++14',str(p/'test.cpp'),'-o',str(p/'test.exe')],check=True)
 subprocess.run([str(p/'test.exe')],check=True)
print('PASS native Wi-Fi C switch, pending guard, audio retry, QR error selection and success continuation')
