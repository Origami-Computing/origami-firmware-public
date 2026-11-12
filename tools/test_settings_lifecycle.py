"""Run actual Settings Main/app transitions with fake services and view destinations.

Preserves initialization attempts and submenu resume behavior; no hardware or NVS.
Optional --source-ref characterizes an earlier implementation against the same flows.
"""
from pathlib import Path
import argparse,re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source-ref');args=p.parse_args()
source=subprocess.check_output(['git','show',args.source_ref+':src/apps/settings_menu_app.cpp'],cwd=ROOT,text=True) if args.source_ref else (ROOT/'src/apps/settings_menu_app.cpp').read_text()
nav=(ROOT/'src/nav_handler.cpp').read_text()
def function(s,name):
 m=re.search(r'(?:static )?void '+name+r'\([^)]*\)\s*\{',s);assert m,name
 b=m.end();depth=1
 while depth:depth+=(s[b]=='{')-(s[b]=='}');b+=1
 return s[m.start():b]
def declaration(marker):
 a=source.index(marker);return source[a:source.index(';',a)+1]
modern='static void prepareWorkflow()' in source
entry='enterMainView' if modern else 'enter'
code=r'''
#include "app.h"
void usageStart(App*, const char*){}
#include <cassert>
#include <cstdio>
#include <initializer_list>
namespace scene {struct Screen {
 const char* name;void(*onEnter)();void(*onExit)();void(*onTick)(uint32_t);
 void(*onInput)(int);bool(*onChord)();void(*onDraw)(TFT_eSprite&);
 const char* id;void(*buildScene)();const uint16_t* color;
};enum class ScreenTransition{Automatic,Forward,Back,Silent};void switchTo(App*,const Screen*);inline void switchTo(App*a,const Screen*s,ScreenTransition){switchTo(a,s);}}
extern App app;
extern const scene::Screen kScreenSettingsMain,kScreenSettingsWifiMenu,kScreenSettingsSoundMenu,kScreenSettingsSleepMenu,kScreenSettingsSensorsMenu;
extern const scene::Screen kScreenSettingsBrightness;
enum Sub{SUB_MAIN,SUB_WIFI_MENU,SUB_SOUND_MENU,SUB_SLEEP_MENU,SUB_SENSORS_MENU,SUB_SLEEP_TIMER,SUB_BRIGHTNESS};
Sub s_sub=SUB_MAIN,s_resumeSub=SUB_MAIN;
bool updateAvailable=false,s_updateNotice=false;
bool s_valueEditing=false,s_preserveMainSelection=false,s_needRedraw=false;
int s_sel=0,attempts=0,refreshes=0,viewEntries=0;bool initOk=true;
namespace services {namespace firmware_update {bool settingsUpdateAvailable(){return updateAvailable;}}}
namespace services {namespace uploads {bool ensureInit(){++attempts;return initOk;}}}
namespace services {namespace screen_brightness {void discard(){}}}
namespace hal {namespace display {void markAllDirty(){}}}
namespace ui {namespace listening_style {void exitPreview(){}} namespace focused_label {void reset(){}}}
AppPresentation presentationPolicy(){return {};}
namespace ui {namespace home_eyes {void setTapHeartsEnabled(bool enabled){assert(!enabled);}}}
void updateRecipeLabel(){}
void updateHapticsLabel(){}
void updateServerLabel(){++refreshes;}void updateSleepLabel(){}void updateOtaChannelLabel(){}
static void showMain();static void showWifiMenu();static void showSoundMenu();static void showSleepMenu();static void showSensorsMenu();
void enterWifiMenu(){s_sub=SUB_WIFI_MENU;++viewEntries;}void enterSoundMenu(){s_sub=SUB_SOUND_MENU;++viewEntries;}
void enterSleepMenu(){s_sub=SUB_SLEEP_MENU;++viewEntries;}void enterSensorsMenu(){s_sub=SUB_SENSORS_MENU;++viewEntries;}
void enterBrightness(){s_sub=SUB_BRIGHTNESS;++viewEntries;}
void screenTick(uint32_t){}void onInput(int){}bool onChord(){return false;}void onDraw(TFT_eSprite&){}void buildMainScene(){}
namespace screen_chrome_layout {const uint16_t kSettingsMainTopBarColor=0;}
struct NavHandler{static NavHandler& instance(){static NavHandler n;return n;}void runtimePolicyChanged(){}void invalidateChrome(){}};
'''
if modern:code+=function(source,'prepareWorkflow')+'\n'
code+=function(source,entry)+'\n'+function(source,'exitApp')+'\n'
code+=declaration('const ::scene::Screen kScreenSettingsMain =')+'\n'
for suffix in ['WifiMenu','SoundMenu','SleepMenu','SensorsMenu']:
 code+='const scene::Screen kScreenSettings'+suffix+'={"sub",enter'+suffix+',nullptr,nullptr,nullptr,nullptr,nullptr,"sub",nullptr,nullptr};\n'
code+='const scene::Screen kScreenSettingsBrightness={"brightness",enterBrightness,nullptr,nullptr,nullptr,nullptr,nullptr,"brightness",nullptr,nullptr};\n'
code+='static const scene::Screen* const kScreens[]={&kScreenSettingsMain,&kScreenSettingsWifiMenu,&kScreenSettingsSoundMenu,&kScreenSettingsSleepMenu,&kScreenSettingsSensorsMenu};\n'
code+=declaration('App app =')+'\n'
for name in ['showMain','showWifiMenu','showSoundMenu','showSleepMenu','showSensorsMenu']:code+=function(source,name)+'\n'
code+=function(nav,'enterApp')+r'''
namespace scene {void switchTo(App* a,const Screen* next){if(a->currentScreen&&a->currentScreen->onExit)a->currentScreen->onExit();a->currentScreen=next;if(next->onEnter)next->onEnter();}}
void overlayReturn(){if(app.onResume)app.onResume();if(app.currentScreen->onEnter)app.currentScreen->onEnter();}
int main(){
 for(bool success:{false,true}){
  initOk=success;s_sel=7;s_preserveMainSelection=false;s_resumeSub=SUB_MAIN;s_valueEditing=true;
  int n=attempts;enterApp(&app,true);
  assert(attempts==n+1&&s_sel==0&&!s_valueEditing&&s_sub==SUB_MAIN&&s_needRedraw);
  s_sel=8;n=attempts;overlayReturn();assert(attempts==n+1&&s_sel==0);
  for(Sub target:{SUB_WIFI_MENU,SUB_SOUND_MENU,SUB_SLEEP_MENU,SUB_SENSORS_MENU}){
   s_resumeSub=target;s_preserveMainSelection=true;s_sel=6;viewEntries=0;n=attempts;
   enterApp(&app);assert(attempts==n+1&&s_sel==6&&s_sub==target&&viewEntries==1&&s_resumeSub==SUB_MAIN);
   n=attempts;overlayReturn();assert(attempts==n&&s_sub==target&&viewEntries==2);
   n=attempts;showMain();assert(attempts==n+1&&s_sel==6&&s_sub==SUB_MAIN);
  }
  n=attempts;app.onExit();assert(attempts==n&&s_sub==SUB_MAIN&&s_resumeSub==SUB_MAIN);
  enterApp(&app,true);assert(attempts==n+1);
 }
'''
if modern:code+='''
 int n=attempts;kScreenSettingsMain.onEnter();assert(attempts==n);
 s_sel=2;scene::switchTo(&app,&kScreenSettingsBrightness);
 n=attempts;enterApp(&app);assert(s_sub==SUB_BRIGHTNESS&&s_sel==2&&s_resumeSub==SUB_MAIN&&attempts==n+1);
 n=attempts;overlayReturn();assert(s_sub==SUB_BRIGHTNESS&&s_resumeSub==SUB_MAIN&&attempts==n);
 showMain();assert(s_sub==SUB_MAIN&&s_sel==2&&s_resumeSub==SUB_MAIN);
 n=attempts;
'''
if modern and 's_updateNotice' in function(source,entry):code+='''
 updateAvailable=true;s_preserveMainSelection=false;kScreenSettingsMain.onEnter();assert(s_updateNotice&&s_sel==-1&&attempts==n);
 updateAvailable=false;s_preserveMainSelection=true;kScreenSettingsMain.onEnter();assert(!s_updateNotice&&s_sel==0&&attempts==n);
'''
code+='puts("PASS Settings activation/uncover/Main return initialization, four category redirects, brightness resume/overlay retention and failed-init retry");}\n'
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory);(p/'test.cpp').write_text(code)
 subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
