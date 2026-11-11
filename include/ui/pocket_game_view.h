#pragma once
// Shared composition for the downloadable pocket games; one scene/flush owner.
#include "ui/scene.h"
#include "hal/hal_display.h"
namespace ui { namespace pocket {
constexpr uint16_t paper=0xF79B,ink=0x21C6,muted=0x9CD3,accent=0x538B;
inline void label(scene::Scene& s,const char* id,Rect r,const char* text,uint16_t fg=ink,uint16_t bg=paper,int size=1){s.addLabel(id,r,text,fg,bg,size,scene::TextAlign::MiddleCenter,3);}
inline void frame(scene::Scene& s,const char* title,const char* hint){s.clear();s.addRect("bg",{0,0,280,240},paper);label(s,"title",{8,38,264,24},title,ink,paper,2);label(s,"hint",{4,220,272,16},hint);}
inline void render(scene::Scene& s,TFT_eSprite& canvas){scene::setCurrentScene(&s);scene::renderCurrentScene(canvas);hal::display::markAllDirty();}
}}
