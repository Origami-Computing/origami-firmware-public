"""Characterize Alarm workflow loading, preview cleanup and editor re-entry.

Runs production entry callbacks, descriptors and NavHandler's entry helper with
fake storage, preview and painting. Does not verify hardware or stored migrations.
"""
from pathlib import Path
import re,subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
s=(ROOT/'src/apps/alarm_clock_app.cpp').read_text()
def function(source,name):
    m=re.search(r'(?:static )?void '+name+r'\([^)]*\)\s*\{',source);assert m,name
    end=m.end();depth=1
    while depth:
        depth+=(source[end]=='{')-(source[end]=='}');end+=1
    return source[m.start():end]
def declaration(prefix):
    a=s.index(prefix);return s[a:s.index(';',a)+1]
code=r'''
#include "app.h"
#include <cassert>
#include <cstdio>
#include <string>
namespace scene {struct Screen {
 const char* name;void(*onEnter)();void(*onExit)();void(*onTick)(uint32_t);
 void(*onInput)(int);bool(*onChord)();void(*onDraw)(TFT_eSprite&);
 const char* id;void(*buildScene)();const uint16_t* statusBarBackground;
};}
extern App app;
extern const scene::Screen kScreenAlarmList,kScreenAlarmEdit;
enum class ScreenMode {List,Edit};
ScreenMode s_mode=ScreenMode::Edit;
bool s_active=false,s_needRedraw=false,s_adjustingField=true,s_editHighlight=false;
bool preview=true;unsigned loads=0,dirty=0;std::string operations;
int draft=123;
struct {int selected=5,scrollFirst=3,totalItems=0;} s_alarmList;
int alarmListRowCount(){return 8;}
namespace services {
namespace vibration {void stopPreview(){if(preview){operations+='V';preview=false;}}}
namespace alarms {void reload(){assert(!preview);operations+='R';++loads;}}
}
namespace hal {namespace display {void markAllDirty(){++dirty;}}}
namespace ui {namespace focused_label {void reset(){}}}
namespace screen_chrome_layout {const uint16_t kAlarmsListTopBarColor=0;}
void screenTick(uint32_t){}void onInput(int){}void onDraw(TFT_eSprite&){}
void buildAlarmListScene(){}void buildAlarmEditScene(){}
// Telemetry is orthogonal to the lifecycle behavior under test.
void usageStart(App*, const char*){}
struct NavHandler {
 static NavHandler& instance(){static NavHandler n;return n;}
 void runtimePolicyChanged(){}void invalidateChrome(){}
};
'''
code+='\n'.join(function(s,n) for n in ['markRedraw','enter','exitApp','enterAlarmList','enterAlarmEdit'])+'\n'
for prefix in ['const ::scene::Screen kScreenAlarmList =','const ::scene::Screen kScreenAlarmEdit =','static const ::scene::Screen* const kScreens','App app =']:
    code+=declaration(prefix)+'\n'
code+=function((ROOT/'src/nav_handler.cpp').read_text(),'enterApp')+'\n'
code+=r'''
void restoreSystemView(){if(app.onResume)app.onResume();app.currentScreen->onEnter();}
int main(){
 enterApp(&app,true);
 assert(s_active && loads==1 && operations=="VR" && s_mode==ScreenMode::List);
 assert(!s_adjustingField && s_alarmList.selected==0 && s_alarmList.scrollFirst==0 && s_alarmList.totalItems==8);
 s_alarmList.selected=4;s_alarmList.scrollFirst=2;preview=true;s_adjustingField=true;
 enterApp(&app,true); // Duplicate descriptor activation: no reload/reset.
 assert(loads==1 && operations=="VRV" && s_alarmList.selected==4 && s_alarmList.scrollFirst==2 && !s_adjustingField);
 app.currentScreen=&kScreenAlarmEdit;app.currentScreen->onEnter();
 assert(s_mode==ScreenMode::Edit && !s_adjustingField && s_editHighlight && draft==123);
 preview=true;s_adjustingField=true;s_editHighlight=false;restoreSystemView();
 assert(loads==1 && preview && !s_adjustingField && s_editHighlight && s_mode==ScreenMode::Edit && draft==123);
 s_active=false;restoreSystemView(); // Existing editor re-entry does not initialize even if inactive.
 assert(!s_active && loads==1 && preview && draft==123);
 enterApp(&app); // Uncover selects list and restores inactive workflow, stopping preview before reload.
 assert(s_active && loads==2 && !preview && operations=="VRVVR" && s_mode==ScreenMode::List);
 s_alarmList.selected=3;preview=true;enterApp(&app);
 assert(loads==2 && s_alarmList.selected==3 && !preview);
 preview=true;app.currentScreen=&kScreenAlarmList;app.currentScreen->onEnter();
 assert(loads==2 && !preview && s_mode==ScreenMode::List); // Ordinary list transition.
 preview=true;app.onExit();assert(!s_active && !preview && loads==2 && draft==123);
 enterApp(&app,true);assert(loads==3 && s_active && s_alarmList.selected==0);
 const auto before=operations;enterApp(nullptr,true);assert(operations==before);
 puts("PASS Alarm activation, duplicate mount, active/inactive editor return, list cleanup ordering, uncover and exit/re-entry");
}
'''
with tempfile.TemporaryDirectory() as d:
    root=Path(d);(root/'main.cpp').write_text(code)
    subprocess.run(['c++','-std=c++11','-fsanitize=address,undefined','-I'+str(ROOT/'include'),str(root/'main.cpp'),'-o',str(root/'test')],check=True)
    subprocess.run([str(root/'test')],check=True)
