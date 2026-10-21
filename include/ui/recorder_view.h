#pragma once
#include <math.h>
#include "ui/timer_controls.h"
#include "ui/raster.h"
#include "ui/pixel_text.h"
#include "assets/kami_dialogue.h"
namespace recorder_view {
// 4px baseline grid; 24px safe margins; 48px character column + 16px gutter.
namespace grid { constexpr int left=24, text=88, hero=72, status=116, wave=184; }
// Expand quiet speech visually while keeping silence flat and loud peaks bounded.
inline int barHeight(int level) {
 const int bounded=level<0?0:level>25?25:level;
 return 2+int(62.0f*sqrtf(bounded/25.0f));
}
template<class C>void render(C& c,int state,unsigned elapsed,unsigned phase,int /*level*/,const char* wave="",unsigned /*position*/=0){
 const uint16_t background=0xFFFF,ink=0x2104;
 const bool active=state==1;
 const bool hasAudio=elapsed>0||wave[0]!=0;
 const bool paused=!active&&(state==2||hasAudio);
 c.fillRect(0,0,280,240,background);
 // A finishes the take; B stays under one finger for capture/pause/resume.
 ui::timer_controls::stop(c,24,16,active);
 if(active){c.fillRect(241,17,5,16,ink);c.fillRect(251,17,5,16,ink);}
 else if(paused){for(int i=0;i<8;i++)c.fillRect(242+i,17+i,1,16-i*2,ink);}
 else{
  c.fillRect(244,15,8,2,ink);c.fillRect(242,17,12,10,ink);c.fillRect(244,27,8,2,ink);
  for(int y=19;y<=23;y+=4)c.fillRect(244,y,8,1,background);
  c.fillRect(239,23,2,6,ink);c.fillRect(255,23,2,6,ink);c.fillRect(241,29,14,2,ink);
  c.fillRect(247,31,2,3,ink);c.fillRect(243,34,10,2,ink);
 }
 const int n=strlen(wave);
 // Anchored idle motion: eyes dip first, body follows, then recover in order.
 const unsigned times[]={0,320,640,1000,1320,1800};
 const int bodyOffsets[]={0,0,2,2,0,0};
 const int eyeOffsets[]={0,1,1,0,0,0};
 const unsigned t=phase%1800;
 int frame=0;while(frame<4&&t>=times[frame+1])++frame;
 float u=float(t-times[frame])/float(times[frame+1]-times[frame]);
 u=u*u*(3-2*u);
 auto offset=[&](const int* values){return active?int(lroundf(values[frame]+(values[frame+1]-values[frame])*u)):0;};
 const int bodyDown=offset(bodyOffsets),eyeDown=offset(eyeOffsets);
 const int x=grid::left+12;
 ui::raster::mask(c,assets::kami_dialogue::kBody,x,88+bodyDown,24,26,0xF945,background,26-bodyDown);
 ui::raster::mask(c,assets::kami_dialogue::kEyes,x+4,95+eyeDown,16,10,ink,0xF945);
 // Recording-only arm, grip and mic follow the body, after the eyes.
 if(active){
 c.fillRect(x+22,106+bodyDown,4,3,0xF945);
 c.fillRect(x+25,104+bodyDown,2,4,0xF945);
 c.fillRect(x+25,106+bodyDown,2,6,ink);
 c.fillRect(x+26,103+bodyDown,2,5,ink);
 c.fillRect(x+27,100+bodyDown,2,4,ink);
 c.fillRect(x+27,103+bodyDown,3,4,0xF945);
 c.fillRect(x+28,104+bodyDown,3,2,0xF945);
 c.fillRect(x+27,96+bodyDown,4,1,ink);
 c.fillRect(x+26,97+bodyDown,6,4,ink);
 c.fillRect(x+27,101+bodyDown,4,1,ink);
 c.fillRect(x+27,97+bodyDown,4,3,0x4208);
 }
 char timer[16];
 snprintf(timer,sizeof(timer),"%02u:%02u",elapsed/60,elapsed%60);
 ui::pixel_text::text(c,timer,grid::text,grid::hero,5,ink);
 const char* label=active?"RECORDING":paused?"PAUSED":"READY";
 ui::pixel_text::text(c,label,grid::text,grid::status,2,ink);
 // Keep the waveform channel visible at rest; pausing preserves its shape.
 c.fillRect(grid::left,grid::wave,232,1,ink);
 for(int i=0;i<n&&i<46;i++){
  const int h=barHeight(wave[i]-'A');
  c.fillRect(grid::left+i*5,grid::wave-h/2,3,h,ink);
 }
}
}
