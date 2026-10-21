#pragma once
#include <cmath>
#include "ui/raster.h"
#include "ui/music_player_view.h"
#include "assets/kami_dialogue.h"
namespace music_kami {
// Compatibility names for existing preview tools; new views use ui::raster.
using ui::raster::blend;
using ui::raster::mask;
template<class C>struct Clip {
 C& canvas;
 void fillRect(int x,int y,int w,int h,uint16_t ink){
  int r=x+w,b=y+h;if(x<24)x=24;if(y<132)y=132;if(r>224)r=224;if(b>150)b=150;
  if(r>x&&b>y)canvas.fillRect(x,y,r-x,b-y,ink);
 }
};
template<class C>void render(C& c,const music_view::State& input,unsigned phase,bool showNotes=false,bool headphones=false){
 auto s=input;s.design=5;music_view::render(c,s);
 c.fillRect(0,40,238,140,kUiBg);
 // Body leads; eyelids follow one beat later, like a soft nod to music.
 const unsigned timing[]={0,300,550,950,1225,1800};
 const int tops[]={68,70,70,67,67,68};
 const int heights[]={26,24,24,27,27,26};
 const int eyes[]={79,79,81,81,78,79};
 unsigned t=phase%1800;int frame=0;
 while(frame<4&&t>=timing[frame+1])++frame;
 double u=double(t-timing[frame])/(timing[frame+1]-timing[frame]);
 u=u*u*(3-2*u);
 auto tween=[&](const int* values){return (int)std::lround(values[frame]+(values[frame+1]-values[frame])*u);};
 const int bodyY=s.playing?tween(tops):68;
 const int bodyH=s.playing?tween(heights):26;
 const int eyeY=s.playing?tween(eyes):79;
 mask(c,assets::kami_dialogue::kBody,24,bodyY,24,26,0xF945,kUiBg,bodyH);
 if(s.playing){
  for(int x=28;x<=38;x+=10){
   c.fillRect(x,eyeY,1,2,kUiFg);
   c.fillRect(x+1,eyeY+1,4,2,kUiFg);
   c.fillRect(x+5,eyeY,1,2,kUiFg);
  }
 }else{
  mask(c,assets::kami_dialogue::kEyes,28,bodyY+7,16,10,kUiFg,0xF945);
 }
 if(headphones){
  const int headphoneY=bodyY-2;
  // Stepped two-pixel headband and padded cups follow the body's motion.
  c.fillRect(30,headphoneY-3,12,2,kUiFg);
  c.fillRect(26,headphoneY-1,4,2,kUiFg);
  c.fillRect(42,headphoneY-1,4,2,kUiFg);
  c.fillRect(24,headphoneY+1,2,5,kUiFg);
  c.fillRect(46,headphoneY+1,2,5,kUiFg);
  c.fillRect(23,headphoneY+6,2,8,kUiFg);
  c.fillRect(47,headphoneY+6,2,8,kUiFg);
  c.fillRect(21,headphoneY+10,5,9,kUiFg);
  c.fillRect(46,headphoneY+10,5,9,kUiFg);
  c.fillRect(22,headphoneY+12,1,5,0x632C);
  c.fillRect(49,headphoneY+12,1,5,0x632C);
 }
 if(showNotes&&s.playing){
  // Native-pixel eighth notes: staggered births, drifting up and to the right.
  const uint16_t colors[]={0xF945,0x05A9,0x249F,0xE680};
  const uint8_t note[]={0x06,0x07,0x05,0x04,0x04,0x1C,0x1C,0x18};
  for(unsigned i=0;i<4;++i){
   const unsigned age=(phase+i*800)%3200;
   if(age>=2600)continue;
   const int x=(headphones?54:47)+(int)(age*31/2600);
   const int y=76-(int)(age*32/2600);
   for(int row=0;row<8;++row)for(int col=0;col<5;++col)
    if(note[row]&(1<<(4-col)))c.fillRect(x+col,y+row,1,1,colors[i]);
  }
 }
 const char* title=s.title[0]?s.title:"Untitled track";
 int overflow=(int)strlen(title)*12-1-200;
 int offset=0;
 if(overflow>0){
  unsigned travel=overflow*40,period=3200+2*travel,t=phase%period;
  if(t<1600)offset=0;
  else if(t<1600+travel)offset=(t-1600)/40;
  else if(t<3200+travel)offset=overflow;
  else offset=overflow-(t-3200-travel)/40;
 }
 Clip<C> clipped{c};
 music_view::text(clipped,title,24-offset,132,2,kUiFg);
#ifndef ORIGAMI_INSTRUMENT_FONT
 // One native-pixel overstrike adds weight to the original bitmap face.
 music_view::text(clipped,title,25-offset,132,2,kUiFg);
#endif
 if(s.artist[0]){char artist[17];music_view::shorten(s.artist,artist,16);music_view::text(c,artist,24,160,2,kUiMuted);}
}
}
