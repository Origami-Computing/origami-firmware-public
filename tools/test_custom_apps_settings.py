"""Run the production app-management workflow with controlled storage completions."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory() as directory:
 p=Path(directory)
 for name in ['hal/hal_display.h','hal/hal_input.h','nav_handler.h','scene/screen.h','ui/list_view.h','ui/nav_controls.h','ui/scene.h','esp_heap_caps.h']:
  f=p/name;f.parent.mkdir(parents=True,exist_ok=True);f.write_text('#pragma once\n')
 (p/'test.cpp').write_text(r'''
#include "app.h"
#include "widgets/storage.h"
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <string>
bool noMemory=false;int allocations=0;
constexpr int MALLOC_CAP_SPIRAM=1,MALLOC_CAP_8BIT=2;
void* heap_caps_malloc(size_t n,int){if(noMemory)return nullptr;++allocations;return malloc(n);}
void heap_caps_free(void* p){if(p)--allocations;free(p);}
class TFT_eSprite{};
namespace hal {namespace input {enum class EventType{Enter,Back,Scroll};struct InputEvent{EventType type;int delta=0;};}namespace display {void markAllDirty(){}}}
namespace ui {
struct Rect{int x,y,w,h;};struct ListItem{const char* label="";int fg=0;bool opensScreen=false;};
namespace scene {enum class TextAlign{TopCenter};struct Scene {std::string message;void clear(){message.clear();}void addRect(const char*,Rect,int){}void addLabel(const char* id,Rect,const char* t,int,int,int,TextAlign){if(!strcmp(id,"apps.message"))message=t;}void render(TFT_eSprite&,int,int){}};Scene& scratchScene(){static Scene s;return s;}}
struct ListView{int selected=0,scrollFirst=0,totalItems=0;void clampScroll(){if(selected>=totalItems)selected=totalItems?totalItems-1:0;}void scroll(int n){selected+=n;if(selected<0)selected=0;clampScroll();}void buildScene(scene::Scene&,int,int,int,int,ListItem(*fn)(int,void*),void* c,int,int,int,int,int){for(int i=0;i<totalItems;++i)assert(fn(i,c).label);}};
namespace nav {bool isBackTap(hal::input::InputEvent e){return e.type==hal::input::EventType::Back;}bool isEnterTap(hal::input::InputEvent e){return e.type==hal::input::EventType::Enter;}}
}
namespace scene {struct Screen{const char* name;void(*enter)();void(*exit)();void(*tick)(uint32_t);void(*input)(hal::input::InputEvent);bool(*chord)();void(*draw)(TFT_eSprite&);const char* id;void(*build)(ui::scene::Scene&,int16_t,int16_t);};}
struct NavHandler{static NavHandler& instance(){static NavHandler n;return n;}void runtimePolicyChanged(){}void pop(){}};
namespace services {namespace custom_apps {const char* resultName(Result){return "storage error";}}}
namespace widgets {namespace storage {
Catalog fixture;Identity removed;Operation last;unsigned next=0,abandoned=0,removes=0;bool busy=false,ready=false;Result result=Result::Ok;
uint32_t request(Operation op,const Identity& id,const uint8_t*,size_t,const char*,const char*){if(busy)return 0;last=op;if(op==Operation::Remove){removed=id;++removes;}return ++next;}
bool take(uint32_t,Result& r,Package*,Catalog* c,Identity*){if(!ready)return false;ready=false;r=result;if(c)*c=fixture;return true;}
void abandon(uint32_t id){abandoned=id;}
}}
'''+'\n#include "'+str(ROOT/'src/apps/custom_apps_settings_app.cpp')+'"\n'+r'''
int main(){using namespace apps::custom_apps_settings;using E=hal::input::EventType;
 noMemory=true;app.onEnter();assert(mode==Mode::Error&&!job);noMemory=false;
 jobs::busy=true;input({E::Enter});assert(mode==Mode::Error&&!job);jobs::busy=false;input({E::Enter});assert(mode==Mode::Loading&&job);
 jobs::fixture.count=3;strcpy(jobs::fixture.entries[0].name,"Lua counter");strcpy(jobs::fixture.entries[0].identity.id,"counter");
 strcpy(jobs::fixture.entries[1].name,"Pong");strcpy(jobs::fixture.entries[1].identity.id,"pong");jobs::fixture.entries[1].native=true;
 jobs::fixture.entries[2].identity.legacy=true;
 jobs::ready=true;tick(1);assert(mode==Mode::List&&catalog->count==2);
 input({E::Enter});assert(mode==Mode::Confirm&&actions.selected==0);input({E::Enter});assert(mode==Mode::List&&!jobs::removes);
 input({E::Scroll,1});input({E::Enter});app.onCover();assert(mode==Mode::List&&!jobs::removes);
 input({E::Enter});input({E::Back});assert(mode==Mode::List&&!jobs::removes);
 input({E::Enter});input({E::Scroll,1});input({E::Enter});assert(mode==Mode::Deleting&&jobs::removes==1&&!strcmp(jobs::removed.id,"pong"));
 jobs::ready=true;tick(2);assert(mode==Mode::Loading);jobs::fixture.count=0;jobs::ready=true;tick(3);assert(mode==Mode::List&&catalog->count==0);
 ui::scene::Scene s;build(s,280,240);assert(s.message=="No installed apps");
 input({E::Enter});assert(!job&&jobs::removes==1);app.onExit();assert(!allocations&&!catalog);
 app.onEnter();unsigned pending=job;app.onExit();assert(jobs::abandoned==pending&&!allocations);
 app.onEnter();jobs::result=custom::Result::Io;jobs::ready=true;tick(4);assert(mode==Mode::Error&&!job);app.onExit();assert(!allocations);
 puts("PASS manage apps: allocation/busy failures, Lua/native list, confirmation, cover/Back, delete, empty state, errors and exit cleanup");
}
''')
 subprocess.run(['c++','-std=c++17','-fsanitize=address,undefined','-I'+str(p),'-I'+str(ROOT/'include'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)
