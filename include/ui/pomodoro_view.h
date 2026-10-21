#pragma once
#include "config/ui_layout.h"
#include <algorithm>
#include "ui/scene.h"
#include "ui/list_view.h"
#include "ui/raster.h"
#include "ui/pixel_text.h"
#include "assets/kami_dialogue.h"
#include "core/alert_vibration.h"
#include "sfx/alarm_catalog.h"
namespace pomodoro_view {
namespace grid { constexpr int left=24,right=256,width=right-left,kamiY=76,
    phaseX=60,phaseY=82,countY=116,progressY=180,captionY=204; }

inline void build(ui::scene::Scene& scene,int mode,int field,bool editing,bool blink,int a,int b,int sound,int buzz,bool rest,int remaining,int total){
 using namespace ui::scene;
  scene.clear();scene.addRect("page",{0,0,280,240},kUiBg);
  auto text=[&](const char* id,const char* value,int x,int y,int w,int h,int size,uint16_t fg=kUiFg){scene.addLabel(id,{(int16_t)x,(int16_t)y,(int16_t)w,(int16_t)h},value,fg,kUiBg,size,TextAlign::MiddleCenter,2);};
  if(mode==4){
   // Use the very same list widget as camera settings: left-aligned mono
   // rows, black selection bar, and the shared blue editing treatment.
   struct Settings { char rows[4][48]; int selected; bool editing,blink; } settings{};
   settings.selected=field;settings.editing=editing;settings.blink=blink;
   snprintf(settings.rows[0],48,"Focus  %02d min",a);
   snprintf(settings.rows[1],48,"Break  %02d min",b);
   snprintf(settings.rows[2],48,"Sound  %s",sfx::alarms::preset(sound%4).name);
   snprintf(settings.rows[3],48,"Buzz  %s",core::alert_vibration::name(buzz));
   ui::ListView list;list.totalItems=4;list.selected=field;
   list.buildScene(scene,0,kSystemStatusBarH,280,240-kSystemStatusBarH,
    [](int i,void* context)->ui::ListItem {
     const auto& state=*static_cast<Settings*>(context);
     ui::ListItem item;item.label=state.rows[i];item.fg=kUiFg;
     item.opensScreen=false;item.editing=state.editing&&i==state.selected;
     item.blinkOn=state.blink;return item;
    },&settings,kUiSelBg,kUiSelFg,kUiBg,kUiMuted,kUiFg);
  }else{
   text("phase",rest?"BREAK":"FOCUS",grid::phaseX,grid::phaseY,152,16,2);
   scene.find("phase")->align=TextAlign::TopLeft;
   char value[32];snprintf(value,32,"%02d:%02d",remaining/60,remaining%60);
   // The mono "1" has a blank leading column. Align visible ink to the
   // same rail as Kami and the progress track, rather than its glyph cell.
   const int countInset=value[0]=='1'?5:0;
   text("count",value,grid::left-countInset,grid::countY,grid::width,40,5);
   scene.find("count")->align=TextAlign::TopLeft;
   // A stable progress channel remains visible before starting, like recorder's baseline.
   scene.addProgressBar("progress",{grid::left,grid::progressY,grid::width,12},mode==0?0:std::max(0,total-remaining),std::max(1,total),0xBDF7,kUiFg,0,1);
   if(mode==0)snprintf(value,32,"%d min %s next",rest?a:b,rest?"focus":"break");
   else snprintf(value,32,"%s",mode==2?"Paused":mode==3?"Session complete":"");
   text("caption",value,grid::left,grid::captionY,grid::width,16,1);
   scene.find("caption")->align=TextAlign::TopLeft;
  }
}
template<class C>void character(C& c,int mode,bool isBreak,unsigned phase){
  // Three restrained motions, driven by time since the state changed.
  // Keep the native bitmap and fixed bottom anchor; eyes lead body movement.
  int bodyY=grid::kamiY,bodyH=26,eyeY=grid::kamiY+7;
  bool closed=false;
  if(mode==1&&!isBreak){
   const unsigned t=phase%4000;
   const bool compressed=t>=1440&&t<3040;
   bodyY+=compressed?1:0;bodyH-=compressed?1:0;
   eyeY+=(t>=1280&&t<2880)?1:0;
   closed=true;
  }else if(mode==3){
   // One completion bounce, then still. Never modulo the completion time.
   if(phase<160)eyeY-=1;
   else if(phase<400){bodyY-=2;eyeY-=2;}
   else if(phase<560){bodyY-=1;eyeY-=1;}
  }else{
   const unsigned t=phase%4800;
   closed=t>=4480&&t<4640;
  }
  ui::raster::mask(c,assets::kami_dialogue::kBody,grid::left,bodyY,24,26,0xF945,kUiBg,bodyH);
  if(closed){
   for(int x=grid::left+4;x<=grid::left+14;x+=10){
    c.fillRect(x,eyeY+3,1,2,kUiFg);c.fillRect(x+1,eyeY+4,4,1,kUiFg);c.fillRect(x+5,eyeY+3,1,2,kUiFg);
   }
  }else ui::raster::mask(c,assets::kami_dialogue::kEyes,grid::left+4,eyeY,16,10,kUiFg,0xF945);
  if(mode==1&&!isBreak){
   // Tiny staggered pixel z's rise into the empty channel above Kami.
   // Confined to x45..58 / y42..66: clear of clock, phase label and controls.
   static const uint8_t z[]={7,1,2,4,7};
   for(unsigned i=0;i<3;i++){
    const unsigned age=(phase+i*1000)%3000;
    if(age>=2400)continue;
    const int x=grid::left+21+int(age*10/2400);
    const int y=grid::kamiY-6-int(age*20/2400);
    const int opacity=age<240?int(age*12/240):age>1600?int((2400-age)*12/800):12;
    const uint16_t ink=ui::raster::blend(kUiFg,kUiBg,opacity);
    for(int row=0;row<5;row++)for(int col=0;col<3;col++)
     if(z[row]&(4>>col))c.fillRect(x+col,y+row,1,1,ink);
   }
  }
}
template<class C>void finish(C& c,int mode,bool rest,unsigned phase,const char* clock){
 // Like camera, draw complete controls directly in screen coordinates. Scene
 // content fitting would independently displace each pixel column of an icon.
 if(mode!=4){
  for(int i=0;i<8;i++)c.fillRect(24+i,16+i,1,16-2*i,kUiFg);
  c.fillRect(35,16,3,16,kUiFg);
  if(mode==1){c.fillRect(242,16,4,16,kUiFg);c.fillRect(251,16,4,16,kUiFg);}
  else for(int i=0;i<8;i++)c.fillRect(244+i,16+i,1,16-2*i,kUiFg);
  static const uint16_t gear[]={0x03c0,0x03c0,0x1bd8,0x3ffc,0x3ffc,0x1e78,0x7c3e,0xfc3f,0xfc3f,0x7c3e,0x1e78,0x3ffc,0x3ffc,0x1bd8,0x03c0,0x03c0};
  for(int y=0;y<16;y++)for(int x=0;x<16;x++)if(gear[y]&(1<<(15-x)))c.fillRect(241+x,53+y,1,1,kUiFg);
 }
 // Clock owns the top bar and must bypass content-only label fitting.
 if(mode==4)ui::pixel_text::text(c,"SETTINGS",24,16,2,kUiFg);
 else ui::pixel_text::centered(c,clock,16,2,kUiFg);
 if(mode!=4)character(c,mode,rest,phase);
}
}
