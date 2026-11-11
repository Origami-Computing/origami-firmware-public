#ifdef MENU_APP
#include "apps/chess_clock_app.h"
#include "scene/screen.h"
#include "ui/scene.h"
#include "hal/hal_display.h"
#include <stdio.h>

namespace apps { namespace chess_clock {
namespace {
constexpr uint16_t paper=0xF79B, ink=0x21C6, muted=0x7BEF, green=0x538B;
constexpr uint32_t durations[]={60,180,300,600,900};
constexpr uint32_t increments[]={0,2,0,5,10};
struct State { uint32_t left[2],last; int preset,player; bool started,running,finished; } s;
void reset(int preset) {
    s={};s.preset=preset;s.left[0]=s.left[1]=durations[preset]*1000;s.last=millis();
}
void enter(){reset(2);}
void tick(uint32_t now){
    const uint32_t delta=now-s.last;s.last=now;
    if(!s.running)return;
    auto& left=s.left[s.player];
    if(delta>=left){left=0;s.running=false;s.finished=true;}else left-=delta;
}
void pause(){tick(millis());s.running=false;}
void input(hal::input::InputEvent ev){
    using E=hal::input::EventType;
    tick(millis());
    if(ev.type==E::Scroll&&!s.started){
        int p=s.preset+ev.delta;if(p<0)p=0;if(p>4)p=4;reset(p);
    }else if(ev.type==E::ButtonC_Tap){
        if(s.finished){reset(s.preset);return;}
        s.started=true;s.running=!s.running;
    }else if(s.running&&((ev.type==E::ButtonA_Tap&&s.player==0)||(ev.type==E::ButtonB_Tap&&s.player==1))){
        s.left[s.player]+=increments[s.preset]*1000;s.player=1-s.player;
    }
}
void build(ui::scene::Scene& view,int16_t width,int16_t height){
    using A=ui::scene::TextAlign;
    view.clear();view.addRect("bg",{0,0,width,height},paper);
    view.addLabel("title",{8,40,264,24},"CHESS CLOCK",ink,paper,2,A::MiddleCenter,1);
    for(int p=0;p<2;++p){
        const bool active=s.started&&p==s.player;
        const uint16_t bg=active?green:ink, fg=paper;
        int16_t x=p?144:12;
        view.addRect(p?"right":"left",{x,76,124,104},bg,12,1);
        view.addLabel(p?"b":"a",{x,84,124,20},p?"B":"A",fg,bg,2,A::MiddleCenter,2);
        char clock[16];uint32_t seconds=(s.left[p]+999)/1000;
        snprintf(clock,sizeof(clock),"%02lu:%02lu",(unsigned long)(seconds/60),(unsigned long)(seconds%60));
        view.addLabel(p?"time-b":"time-a",{x,108,124,44},clock,fg,bg,3,A::MiddleCenter,2);
        view.addLabel(p?"turn-b":"turn-a",{x,152,124,20},active?(s.finished?"TIME":"YOUR TURN"):"",fg,bg,1,A::MiddleCenter,2);
    }
    char setting[40];snprintf(setting,sizeof(setting),"%lu min  +%lus per move",(unsigned long)(durations[s.preset]/60),(unsigned long)increments[s.preset]);
    view.addLabel("setting",{8,188,264,16},setting,ink,paper,1,A::MiddleCenter,1);
    const char* hint=s.finished?"C new game":!s.started?"Wheel time  /  C start A":s.running?"A / B end turn  /  C pause":"Paused  /  C resume";
    view.addLabel("hint",{8,212,264,16},hint,ink,paper,1,A::MiddleCenter,1);
}
void draw(TFT_eSprite& canvas){auto& view=ui::scene::scratchScene();build(view,280,240);ui::scene::setCurrentScene(&view);ui::scene::renderCurrentScene(canvas);hal::display::markAllDirty();}
const ::scene::Screen screen={"Chess clock",nullptr,nullptr,tick,input,nullptr,draw,"clock",build,&paper};
const ::scene::Screen* const screens[]={&screen};
}
App app={"Chess clock",&screen,nullptr,nullptr,pause,"chess-clock",
         "src/apps/chess_clock_app.cpp",screens,1,nullptr,nullptr,enter,nullptr,nullptr,pause};
}}
#endif
