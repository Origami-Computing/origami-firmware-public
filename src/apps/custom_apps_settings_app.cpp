#include "apps/custom_apps_settings_app.h"
#include "widgets/storage.h"
#include "config/ui_theme.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "ui/list_view.h"
#include "ui/nav_controls.h"
#include "ui/scene.h"
#include <esp_heap_caps.h>
#include <new>
#include <cstdio>

namespace apps { namespace custom_apps_settings {
namespace {
namespace jobs=widgets::storage;
namespace custom=services::custom_apps;
enum class Mode { Loading, List, Confirm, Deleting, Error };
Mode mode=Mode::Loading;
custom::Catalog* catalog=nullptr;
custom::Identity selected;
char selectedName[80]{},message[80]{};
uint32_t job=0;
ui::ListView list,actions;
void show(Mode next){mode=next;NavHandler::instance().runtimePolicyChanged();hal::display::markAllDirty();}
void fail(const char* text){snprintf(message,sizeof(message),"%s",text);show(Mode::Error);}
void refresh(){
 if(!catalog){
  void* memory=heap_caps_malloc(sizeof(custom::Catalog),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(!memory){fail("Not enough memory. Try again");return;}
  catalog=new(memory) custom::Catalog{};
 }
 job=jobs::request(jobs::Operation::Catalog);
 if(!job){fail("Storage busy. Try again");return;}
 show(Mode::Loading);
}
void enter(){list.selected=list.scrollFirst=0;refresh();}
void exit(){if(job)jobs::abandon(job);job=0;heap_caps_free(catalog);catalog=nullptr;}
void cover(){if(mode==Mode::Confirm)show(Mode::List);}
void tick(uint32_t){
 if(!job)return;
 custom::Result result;
 if(!jobs::take(job,result,nullptr,mode==Mode::Loading?catalog:nullptr))return;
 job=0;
 if(result!=custom::Result::Ok){fail(custom::resultName(result));return;}
 if(mode==Mode::Deleting){refresh();return;}
 // Legacy SD scripts are read-only through the persistent package owner.
 size_t kept=0;
 for(size_t i=0;i<catalog->count;++i)if(!catalog->entries[i].identity.legacy)catalog->entries[kept++]=catalog->entries[i];
 catalog->count=kept;list.totalItems=int(kept);
 if(list.selected>=int(kept))list.selected=kept?int(kept)-1:0;
 list.clampScroll();show(Mode::List);
}
void input(hal::input::InputEvent ev){
 if(ui::nav::isBackTap(ev)){
  if(mode==Mode::Confirm){show(Mode::List);return;}
  NavHandler::instance().pop();return;
 }
 if(ev.type==hal::input::EventType::Scroll){
  if(mode==Mode::List)list.scroll(ev.delta);
  if(mode==Mode::Confirm)actions.scroll(ev.delta);
  hal::display::markAllDirty();return;
 }
 if(!ui::nav::isEnterTap(ev))return;
 if(mode==Mode::Error){refresh();return;}
 if(mode==Mode::List&&catalog&&catalog->count){
  const auto& entry=catalog->entries[list.selected];selected=entry.identity;
  snprintf(selectedName,sizeof(selectedName),"%s",entry.name);
  actions.selected=actions.scrollFirst=0;actions.totalItems=2;show(Mode::Confirm);return;
 }
 if(mode==Mode::Confirm){
  if(actions.selected==0){show(Mode::List);return;}
  job=jobs::request(jobs::Operation::Remove,selected);
  if(!job){fail("Storage busy. Try again");return;}
  show(Mode::Deleting);
 }
}
ui::ListItem item(int index,void*){ui::ListItem row;row.label=catalog->entries[index].name;row.fg=kUiFg;row.opensScreen=true;return row;}
ui::ListItem action(int index,void*){ui::ListItem row;row.label=index?"Delete app":"Cancel";row.fg=kUiFg;return row;}
void build(ui::scene::Scene& s,int16_t,int16_t){
 s.clear();s.addRect("apps.bg",{0,0,280,240},kUiBg);
 if(mode==Mode::List&&catalog&&catalog->count){
  list.buildScene(s,0,40,280,178,item,nullptr,kUiSelBg,kUiSelFg,kUiBg,kUiMuted,kUiFg);return;
 }
 const char* title=mode==Mode::Confirm?"Delete app?":"Manage apps";
 const char* text=mode==Mode::Loading?"Loading apps...":mode==Mode::Deleting?"Deleting app...":mode==Mode::Error?message:mode==Mode::Confirm?selectedName:"No installed apps";
 s.addLabel("apps.title",{16,56,248,28},title,kUiFg,kUiBg,2,ui::scene::TextAlign::TopCenter);
 s.addLabel("apps.message",{16,96,248,24},text,kUiMuted,kUiBg,1,ui::scene::TextAlign::TopCenter);
 if(mode==Mode::Confirm){
  actions.buildScene(s,0,128,280,80,action,nullptr,kUiSelBg,kUiSelFg,kUiBg,kUiMuted,kUiFg);
  s.addLabel("apps.keep-data",{16,220,248,16},"Saved app data is kept",kUiMuted,kUiBg,1,ui::scene::TextAlign::TopCenter);
 }else if(mode==Mode::Error)s.addLabel("apps.retry",{16,176,248,20},"C: retry   A: back",kUiFg,kUiBg,1,ui::scene::TextAlign::TopCenter);
}
void draw(TFT_eSprite& canvas){auto& s=ui::scene::scratchScene();build(s,280,240);s.render(canvas,0,0);hal::display::markAllDirty();}
core::health::RuntimePolicy policy(){return core::health::whileBusy(mode==Mode::Loading||mode==Mode::Deleting);}
const ::scene::Screen screen={"Manage apps",nullptr,nullptr,tick,input,nullptr,draw,"manage-apps",build};
const ::scene::Screen* const screens[]={&screen};
}
App app=[] {App a=SCENE_APP_CATALOG_LIFECYCLE_POLICIES("Manage apps","manage-apps",&screen,enter,exit,"src/apps/custom_apps_settings_app.cpp",screens,nullptr,policy);a.onCover=cover;return a;}();
} }
