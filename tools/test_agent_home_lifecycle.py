"""Exercise production Agent Home setup, resume selection and stage transitions."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
def function(path,signature):
    source=(ROOT/path).read_text();start=source.index(signature+' {')
    end=source.index('{',start)+1;depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[start:end]
app=(ROOT/'src/apps/agent_app.cpp').read_text()
descriptor=app[app.index('App app = [] {'):app.index('}();',app.index('App app = [] {'))+4]
code=r'''
#include "app.h"
#include "apps/agent/agent_state.h"
#include "scene/screen.h"
#include <cassert>
#include <vector>
#include <cstring>
using namespace apps::agent;
AgentState s_state;
AgentState& S(){return s_state;}
std::vector<int> events;
namespace motion {void prepareHome(){}void startHome(){}void tick(AgentState&,uint32_t){}void beforeStage(Stage,Stage){}}
namespace ui {namespace home_motion {struct Model {}; } namespace home_eyes {void setHomeMotion(int,bool){} } namespace listening_style {void prepareCache(){}bool prepare(){return true;}}}
AppPresentation presentationPolicy(){return {};}

ui::home_motion::Model character;uint32_t motionAt=0;bool gravityReady=false;
bool workerReady=false,failFirstStart=false;unsigned ensureCalls=0;
uint32_t sConnHintEnterMs=0,sConnReadyAtMs=0;bool sConnWasReady=false;
uint32_t millis(){return 100;}
struct Logger{void println(const char*){}} Serial;
#define LOGD(...) ((void)0)
namespace services{namespace agent_latency{void fail(){}void cancel(){}void replyReady(){}}namespace agent_voice{void resetForEntry(){events.push_back(0);}}}
namespace render{void paintRecordingBorder(AgentState&,uint32_t,bool){}void releaseChatSprites(AgentState&){events.push_back(1);}void paintEyesHomeStatic(AgentState&){events.push_back(5);}}
namespace sfx{void loadVolume(){events.push_back(6);}}
namespace hal{namespace wifi{bool online=true;bool connected(){return online;}bool connect(){events.push_back(3);return true;}}}
void goTo(Stage);
int dismissed=0,cancelled=0;bool sessionActive=false;
namespace services{namespace agent_transport{void cancel(uint8_t){++cancelled;}}}
bool waiting=false;
namespace flow{bool waitingForDelegation(){return waiting;}void cancelDelegation(){++dismissed;}bool isSessionActive(){return sessionActive;}void goTo(Stage s){::goTo(s);}AgentState& state(){return s_state;}bool ensureTransport(bool reconnectIfOffline=false){if(reconnectIfOffline&&!hal::wifi::connected())hal::wifi::connect();events.push_back(2);++ensureCalls;if(!workerReady){if(failFirstStart&&ensureCalls==1)return false;workerReady=true;}return true;}}
struct NavHandler{static NavHandler& instance(){static NavHandler n;return n;}void runtimePolicyChanged(){events.push_back(4);}};
void stopEyesFeedback(){}
void enterOtherView(){events.push_back(5);}
'''
code+=function('src/apps/agent/screen_eyes.cpp','void prepareEyesView(AgentState& s)')+'\n'
code+=function('src/apps/agent_app.cpp','void enterEyesScreen()')+'\n'
code+=function('src/apps/agent/screen_recording.cpp','void onExit(AgentState& s)').replace('void onExit(', 'void recordingExit(')+'\n'
code+=function('src/apps/agent/screen_error.cpp','void onExit(AgentState& s)').replace('void onExit(', 'void errorExit(')+'\n'
code+=r'''
void leaveRecording(){recordingExit(s_state);}void leaveError(){errorExit(s_state);}
const scene::Screen kScreenEyes={enterEyesScreen,nullptr};
const scene::Screen kScreenRecording={enterOtherView,leaveRecording};
const scene::Screen kScreenThinking={enterOtherView,nullptr};
const scene::Screen kScreenReply={enterOtherView,nullptr};
const scene::Screen kScreenError={enterOtherView,leaveError};
const scene::Screen* const kScreens[]={&kScreenEyes,&kScreenRecording,&kScreenThinking,&kScreenReply,&kScreenError};
const scene::Screen& appScreenFor(Stage s){return *kScreens[(unsigned)s];}
extern App app;
void exitApp(){}void preTick(uint32_t){}void prepareForSleep(){}
core::health::RuntimePolicy runtimePolicy(){return {};}
'''
code+=function('src/apps/agent_app.cpp','void prepareHome()')+'\n'
code+=function('src/apps/agent_app.cpp','static void resumeWorkflow()')+'\n'
code+=descriptor+'\n'+function('src/apps/agent/agent_flow.cpp','void goTo(Stage next)')+'\n'
code+=r'''
void dirty(Stage stage){
 workerReady=false;failFirstStart=false;ensureCalls=0;
 s_state.stage=stage;s_state.prevStageForIdle=Stage::Thinking;s_state.sendJustFinished=true;
 s_state.chordHoldDeadlineMs=123;s_state.animFrame=99;
 strcpy(s_state.errBuf,"old");app.currentScreen=kScreens[(unsigned)stage];events.clear();
}
void homeState(){assert(s_state.stage==Stage::Eyes&&s_state.prevStageForIdle==Stage::Eyes);
 assert(!s_state.sendJustFinished&&s_state.chordHoldDeadlineMs==0&&s_state.animFrame==0);}
int main(){
 assert(app.onEnter&&app.onResume&&app.onPreTick&&app.onPrepareSleep&&app.onCover);
 app.onCover();assert(dismissed==1&&cancelled==0);sessionActive=true;app.onCover();assert(dismissed==2&&cancelled==1);sessionActive=false;
 dirty(Stage::Eyes);app.onEnter();app.currentScreen->onEnter();homeState();
 assert((events==std::vector<int>{0,1,2,4,5,6,2}));
 hal::wifi::online=false;events.clear();app.onEnter();assert((events==std::vector<int>{0,1,3,2,4}));hal::wifi::online=true;
 for(unsigned stage=0;stage<5;++stage){
  dirty((Stage)stage);app.onResume();app.currentScreen->onEnter();
  if(stage==0){homeState();assert((events==std::vector<int>{0,1,2,4,5,6,2}));}
  else{assert(s_state.stage==(Stage)stage&&s_state.sendJustFinished);assert((events==std::vector<int>{5}));}
  // Uncover/root refresh select the initial view even if workflow was elsewhere.
  dirty((Stage)stage);app.currentScreen=app.initialScreen;app.onResume();homeState();
  assert((events==std::vector<int>{0,1,2,4}));
  assert(strcmp(s_state.errBuf,"old")==0); // Uncover does not exit the covered Error view.
  for(unsigned next=0;next<5;++next){
   dirty((Stage)stage);goTo((Stage)next);
   if(next==stage){assert(events.empty());continue;}
   if(next==0){homeState();assert((events==std::vector<int>{4,0,1,2,4,5,6,2}));}
   else{assert(s_state.stage==(Stage)next);assert((events==std::vector<int>{4,5}));}

   if(stage==4)assert(!s_state.errBuf[0]);
  }
 }
 // Returning to Eyes keeps a delegated network reservation; no voice reset.
 dirty(Stage::Eyes);waiting=true;app.onResume();homeState();assert((events==std::vector<int>{1,2,4}));waiting=false;
 // View implementation has no service preparation; app-owned entry coordinates it.
 dirty(Stage::Eyes);prepareEyesView(s_state);assert((events==std::vector<int>{5}));
 assert(ensureCalls==0&&s_state.sendJustFinished&&s_state.animFrame==99);
 // Eyes entry does not reset workflow or join Wi-Fi, but DOES ensure transport.
 dirty(Stage::Eyes);hal::wifi::online=false;kScreenEyes.onEnter();
 assert((events==std::vector<int>{5,6,2}));hal::wifi::online=true;
 assert(s_state.sendJustFinished&&s_state.animFrame==99);
 assert(s_state.miniEyesOpen&&sConnHintEnterMs==100&&sConnReadyAtMs==0&&!sConnWasReady);
 // Returning from notice Settings restores the notice instead of silently returning Home.
 dirty(Stage::Eyes);s_state.resumeNotice=true;kScreenEyes.onEnter();
 assert(!s_state.resumeNotice && s_state.stage==Stage::Error);
 assert(app.currentScreen==&kScreenError);
 assert((events==std::vector<int>{4,5}));
 // A failed app-level start gets a second attempt after view/volume setup.
 dirty(Stage::Eyes);failFirstStart=true;app.onEnter();assert(ensureCalls==1&&!workerReady);
 kScreenEyes.onEnter();assert(ensureCalls==2&&workerReady);
 assert((events==std::vector<int>{0,1,2,4,5,6,2}));
}
'''
with tempfile.TemporaryDirectory() as d:
    root=Path(d);(root/'scene').mkdir();(root/'sfx').mkdir()
    (root/'services').mkdir(exist_ok=True)
    (root/'services/vibration.h').write_text('#pragma once\nnamespace services{namespace vibration{enum class Cue{Select,Back};inline void navigationFeedback(Cue){}inline void screenFeedback(){}}}\n')
    (root/'scene/screen.h').write_text('#pragma once\nstruct App;namespace scene{struct Screen{void(*onEnter)();void(*onExit)();};enum class ScreenTransition{Automatic,Forward,Back,Silent};void switchTo(App*,const Screen*);void switchTo(App*,const Screen*,ScreenTransition);}\n')
    (root/'sfx/audio_group.h').write_text('namespace sfx{inline void setActiveAudioGroup(void*){}}\n')
    (root/'test.cpp').write_text(code)
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(root),'-I'+str(ROOT/'include'),str(root/'test.cpp'),str(ROOT/'src/scene/screen.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
print('PASS activation, offline reconnect, resume by interrupted view, all stage transitions, actual Eyes entry and post-view transport retry')
