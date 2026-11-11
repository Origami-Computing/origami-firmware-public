"""Test Tutorial's actual lifecycle hooks/descriptor against the real flow model.

Persistence and Wi-Fi result delivery are modeled; drawing is not executed.
"""
from pathlib import Path
import subprocess,os,shlex
import tempfile

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT/'src/apps/tutorial_app.cpp').read_text()

def function(name):
    start = source.index('void '+name+'(){')
    end = source.index('{', start)+1
    depth = 1
    while depth:
        depth += (source[end]=='{')-(source[end]=='}')
        end += 1
    return source[start:end]

descriptor = next(line for line in source.splitlines() if line.startswith('App app ='))
screen = next(line for line in source.splitlines() if line.startswith('const ::scene::Screen kScreen='))
code = r'''
#include "app.h"
#include "apps/onboarding_flow.h"
#include "apps/onboarding_controls.h"
#include "services/photo_storage.h"
#include <cassert>
#include <cstring>
namespace scene {
struct Screen {
 const char* name; void (*onEnter)(); void (*onExit)();
 void (*onTick)(uint32_t); void (*onInput)(int); bool (*onChord)();
 void (*onDraw)(TFT_eSprite&); const char* id; void (*buildScene)();
};
}
namespace ui {void setNativeFrameOverlay(void(*)(uint16_t*)) {}}
static void compositeNavigationHint(uint16_t*){}
using apps::onboarding::Flow;
using apps::onboarding::WifiResult;
Flow s_flow;
unsigned s_usbSuccessAt=0;bool s_controlSuccessDrawn=false;bool s_catalogRequested=false;
namespace core {namespace secrets {struct Config{char apiKey[2]={};};Config& backendConfig(){static Config c;return c;}}}
namespace hal {namespace wifi {bool connected(){return false;}}namespace input {bool cDown=false;bool buttonCDown(){return cDown;}}}
namespace services {namespace vibration {enum class Cue{Scroll};unsigned ticks=0;void navigationFeedback(Cue){++ticks;}}}
namespace apps {namespace app_store {enum class TutorialResult{None,Back,Skipped,Installed};TutorialResult takeTutorialResult(){return TutorialResult::Skipped;}}}

apps::onboarding::Controls s_controls;
bool s_preview=false;bool s_active=false,s_frozen=true,s_dirty=false;
bool s_reviewCached=true,s_photoQueued=true;
services::photos::Target s_photo,photoResult;
struct {unsigned releases=0;void release(){++releases;}} s_photoTransport;
unsigned photoTakes=0,copyRefreshes=0,motionPreparations=0;
void refreshCopy(){++copyRefreshes;}
void prepareMotion(){++motionPreparations;}
struct {void println(const char*){}} Serial;
namespace apps {namespace camera_menu {
services::photos::Target takeTutorialPhoto(){++photoTakes;auto result=photoResult;photoResult={};return result;}
}}
unsigned s_started=0,s_lastDraw=99,now=1234,loads=0,saves=0,takes=0,savedPage=99;
apps::onboarding::TextPlayback s_text;
unsigned s_speechLetters=0,s_speechBlips=0,s_scanAfter=0;
int s_scrollFrom=0,s_scrollTarget=0;uint32_t s_scrollAt=0;
constexpr unsigned kScanReadingPauseMs=2200;
unsigned readyAt(){return 100;}void stopSpeech(){}void releaseDialogue(){}
unsigned millis(){return now;}
bool load(){++loads;s_flow.page=8;return true;}
void save(){++saves;savedPage=s_flow.page;}
WifiResult wifiResult=WifiResult::None;
namespace apps {namespace wifi_setup {
WifiResult takeTutorialResult(){++takes;auto result=wifiResult;wifiResult=WifiResult::None;return result;}
}}
void exitApp(){} void tick(uint32_t){} void onInput(int){} bool onChord(){return true;}
void draw(TFT_eSprite&){} void buildScene(){}
AppPresentation presentationPolicy(){return {};}
core::health::RuntimePolicy runtimePolicy(){return {};}
'''
sample = source[source.index('void tick(uint32_t now){'):source.index(' if(s_flow.wifiOnly && s_flow.page==Flow::kFinished)')]+'}\n'
code = code.replace('void tick(uint32_t){}',sample)
code += '\n'.join(function(name) for name in ['enterView','changed','prepareWorkflow'])
code += '\n'+screen+'\nconst scene::Screen* const kScreens[]={&kScreen};\n'+descriptor
code += r'''
void activate(){app.currentScreen=app.initialScreen;app.onEnter();app.currentScreen->onEnter();}
void resume(){app.onResume();app.currentScreen->onEnter();}
int main(){
 s_flow.page=5;s_controls.reset(5);hal::input::cDown=true;tick(0);tick(1500);
 assert(s_controls.held&&!s_controls.success&&services::vibration::ticks==0);
 hal::input::cDown=false;tick(1600);tick(1700);
 assert(s_controls.success&&!s_controls.held&&services::vibration::ticks==1);
 s_flow.reset();
 assert(app.onEnter&&app.onResume&&app.initialScreen->onEnter);
 activate();assert(s_active&&loads==1&&saves==1&&s_flow.page==0&&savedPage==0);
 assert(!s_frozen&&s_dirty&&s_lastDraw==0&&s_started==now);
 // Boot offer has already loaded a checkpoint; activation must retain it.
 for(unsigned page=0;page<=49;++page){
  if(page==12||page==19||page==30)continue;
  s_flow.page=page;const auto before=loads;activate();
  assert(loads==before&&s_flow.page==page&&savedPage==page);
 }
 // A presentation-only re-entry must not consume workflow results or save.
 for(unsigned page=0;page<=49;++page){
  s_flow.page=page;s_flow.practiced=true;wifiResult=WifiResult::Joined;
  const auto oldSaves=saves,oldTakes=takes,oldLoads=loads,oldPhotos=photoTakes;
  app.currentScreen->onEnter();
  assert(s_flow.page==page&&s_flow.practiced&&saves==oldSaves&&takes==oldTakes&&loads==oldLoads);
  assert(photoTakes==oldPhotos&&!s_reviewCached);
 }
 const WifiResult results[]={WifiResult::None,WifiResult::Back,WifiResult::Skipped,WifiResult::Failed,WifiResult::Joined};
 const unsigned pages[]={41,41,41,41,120};
 for(unsigned i=0;i<5;++i){
  s_flow.page=12;wifiResult=results[i];const auto before=takes;resume();
  assert(s_flow.page==pages[i]&&savedPage==pages[i]&&takes==before+1);
  resume();assert(takes==before+1); // A subsequent overlay return cannot consume twice.
 }
 for(bool practiced:{false,true}){
  s_flow.page=19;s_flow.practiced=practiced;resume();
  assert(s_flow.page==(practiced?122u:18u)&&savedPage==s_flow.page);
 }
 s_flow.page=19;s_flow.account=true;s_flow.connected=true;s_flow.practiced=true;resume();assert(s_flow.page==25);
 s_flow.account=false;s_flow.connected=false;
 for(bool captured:{false,true}) {
  s_flow.page=30;s_photoQueued=true;photoResult={};const auto releases=s_photoTransport.releases;
  if(captured){photoResult.sequence=42;std::strcpy(photoResult.path,"/photos/test-fixture.jpg");}
  const auto before=photoTakes;resume();
  assert(s_flow.page==(captured?31u:40u)&&savedPage==s_flow.page&&photoTakes==before+1);
  if(captured)assert(s_photo.sequence==42&&s_photo.valid()&&!s_photoQueued);
  assert(s_photoTransport.releases==releases+(captured?0:1));
  resume();assert(photoTakes==before+1);
 }
 s_flow.page=7;const auto before=saves;changed();assert(saves==before+1&&savedPage==7);
}
'''
with tempfile.TemporaryDirectory(dir=ROOT/'.cache') as directory:
    path=Path(directory)
    (path/'test.cpp').write_text(code)
    subprocess.run(shlex.split(os.environ.get('CXX','c++'))+['-std=c++14','-DORIGAMI_DEV_SERIAL_INPUT','-include','initializer_list',
                    '-I'+str(ROOT/'include'),
                    str(path/'test.cpp'),'-o',str(path/'test.exe')],check=True)
    subprocess.run([str(path/'test.exe')],check=True,timeout=10)
print('PASS Tutorial activation/checkpoint retention, view-only refresh, Wi-Fi/menu/Camera returns, one-shot photo handoff and workflow save')
