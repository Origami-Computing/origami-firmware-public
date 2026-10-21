#pragma once
#include "config/ui_layout.h"
// Shared device/preview renderer; native RGB565 animation frames on the MCU.
// All output is RGB565 at 280x240; no browser font or enlarged tiny sprites.
#if defined(ARDUINO) || defined(ONBOARDING_BAKED_TEST)
#include "assets/onboarding_animations.h"
#endif
#include <cmath>
#include <algorithm>
#include "assets/boot_double_blink_kami.h"
#include "ui/raster.h"
#include "ui/pixel_text.h"
#include "assets/kami_dialogue.h"
#include "ui/pixel_dialogue_frame.h"
#include "ui/dial_icon.h"

namespace ui { namespace onboarding_intro {
constexpr unsigned kMoveAt = 2400, kArriveAt = 3400;
constexpr unsigned kEntranceMs = 900;
constexpr unsigned kCharacterMs = 56; // 25% more characters per second than 70 ms.
constexpr unsigned kTextAt = 3900, kReadyAt = kTextAt + 15*kCharacterMs + 140;
constexpr unsigned kPromptFadeMs = 700;
constexpr uint16_t kRed = 0xF945;
inline float clamp(float x) { return x<0?0:x>1?1:x; }
inline float ease(float x) { x=clamp(x); return x*x*x*(x*(x*6-15)+10); }
inline int mix(int a,int b,float u) { return int(std::lround(a+(b-a)*u)); }
inline int alpha(const uint8_t* data,int w,int x,int y) {
 const int i=y*w+x; return (pgm_read_byte(data+i/2)>>(i%2?0:4))&15;
}
// Exact box coverage for reductions. Fractional placement remains antialiased;
// each destination pixel is sampled once from the original native alpha mask.
template<class C> void mask(C& c,const uint8_t* data,int sw,int sh,
 float x,float y,float w,float h,uint16_t ink,uint16_t paper) {
 for(int py=std::max(0,int(std::floor(y)));py<std::min(240,int(std::ceil(y+h)));++py) {
  int run=-1; uint16_t previous=paper;
  const int left=std::max(0,int(std::floor(x))),right=std::min(280,int(std::ceil(x+w)));
  for(int px=left;px<=right;++px) {
   float sum=0;
   if(px<right) {
    const float x0=(px-x)*sw/w,x1=(px+1-x)*sw/w;
    const float y0=(py-y)*sh/h,y1=(py+1-y)*sh/h;
    for(int sy=std::max(0,int(std::floor(y0)));sy<std::min(sh,int(std::ceil(y1)));++sy)
     for(int sx=std::max(0,int(std::floor(x0)));sx<std::min(sw,int(std::ceil(x1)));++sx)
      sum+=alpha(data,sw,sx,sy)*(std::min(x1,float(sx+1))-std::max(x0,float(sx)))*(std::min(y1,float(sy+1))-std::max(y0,float(sy)));
    sum/=((x1-x0)*(y1-y0));
   }
   const uint16_t color=ui::raster::blend(ink,paper,int(std::lround(sum)));
   if(run>=0&&(color!=previous||px==right)) { c.fillRect(run,py,px-run,1,previous);run=-1; }
   if(px<right&&color!=paper&&run<0)run=px;
   previous=color;
  }
 }
}
template<class C> void hand(C& c,float cx,float cy,float scale) {
 // Round palm with approximately the same area as the original oval.
 const float radius=7.5f*scale;
 const int bound=int(std::ceil(radius+1));
 for(int y=std::max(0,int(cy)-bound);y<=std::min(239,int(cy)+bound);++y)
  for(int x=std::max(0,int(cx)-bound);x<=std::min(279,int(cx)+bound);++x){
  int hits=0;
  for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx){
   const float dx=x+(sx+.5f)/4-cx,dy=y+(sy+.5f)/4-cy;
   if(dx*dx+dy*dy<=radius*radius)++hits;
  }
  if(hits)c.fillRect(x,y,1,1,ui::raster::blend(kRed,kUiBg,(hits*15+8)/16));
 }
}
template<class C> struct Fade {
 C& target; int opacity;
 void fillRect(int x,int y,int w,int h,uint16_t color) {
  target.fillRect(x,y,w,h,ui::raster::blend(color,kUiBg,opacity));
 }
};
// Continue the double-rule frame into a pointed upper-left speech corner.
// The other three corners keep the existing speaking-screen silhouette.
using pixel_dialogue::speechCorner;

inline unsigned lineCount(const char* text) {
 unsigned lines=1;for(;*text;++text)if(*text=='\n')++lines;return lines;
}
template<class C> void textViewport(C& c,const char* source,unsigned count,unsigned scroll=999,int x=38,int y=88,unsigned rows=4,bool centered=false,int linePitch=22) {
 const unsigned total=lineCount(source),limit=total>rows?total-rows:0;
 if(scroll==999){
  unsigned shownLine=0,remaining=count;
  for(const char* p=source;*p&&remaining;++p,--remaining)if(*p=='\n')++shownLine;
  scroll=shownLine>=rows?shownLine-rows+1:0;
 }
 scroll=std::min(scroll,limit);unsigned lineNo=0;
 while(*source){
  char line[32]={0};unsigned n=0;
  while(*source&&*source!='\n'){if(count&&n<31)line[n++]=*source;if(count)--count;++source;}
  if(lineNo>=scroll&&lineNo<scroll+rows){
   if(centered)ui::pixel_text::centered(c,line,y+(lineNo-scroll)*linePitch,2);
   else ui::pixel_text::text(c,line,x,y+(lineNo-scroll)*linePitch,2,kUiFg);
  }
  if(*source=='\n'){++source;if(count)--count;}++lineNo;
 }
 if(limit){
  c.fillRect(246,y+4,2,rows*linePitch-12,kUiMuted);
  c.fillRect(245,y+4+int(scroll)*(int(rows)*linePitch-28)/int(limit),4,16,kUiFg);
  // A visible down/up chevron means more text is available via the wheel.
  const int tip=scroll<limit?y+rows*linePitch-3:y;
  for(int i=0;i<3;++i)c.fillRect(244+i,scroll<limit?tip+i:tip-i,6-2*i,1,kUiFg);
 }
}
template<class C> void prompt(C& c,unsigned age,const char* first,const char* second="") {
 const float opacity=age<kPromptFadeMs?(11.f/15)*ease(float(age)/kPromptFadeMs)
  :(8.5f+2.5f*std::cos((age-kPromptFadeMs)*6.2831853/2600))/15;
 const int g5=int(std::lround(31*(1-opacity))),g6=int(std::lround(63*(1-opacity)));
 const uint16_t ink=(g5<<11)|(g6<<5)|g5;
 ui::pixel_text::centered(c,first,194,2,ink);ui::pixel_text::centered(c,second,214,2,ink);
}
inline const char* dialogue(unsigned page) {
 static const char* lines[]={"Hi, I'm Kami...", "I'm your personal\ndigital sidekick,\nlike Bmo or\nJarvis...", "There's a lot\nthat I can do, so\nlet me give you\na tour.", "This is my A\nbutton. Press it\nto continue.", "This is my B\nbutton. Press it\nto continue.", "This is my C\nwheel. Press it\nto continue.", "You can also\nrotate the C\nwheel. Give it a\ntry to continue."};
 return lines[std::min(page,6u)];
}
inline unsigned textAt(unsigned page) { return page?450:kTextAt; }
inline unsigned readyAt(unsigned page) { return textAt(page)+unsigned(strlen(dialogue(page)))*kCharacterMs+140; }
template<class C> void typedDialogue(C& c,unsigned ms,unsigned page) {
 if(ms<textAt(page))return;
 textViewport(c,dialogue(page),(ms-textAt(page))/kCharacterMs);
}

// A curved shoulder wraps around the case; the lower-right cutout is the screen.
// Composite every subpixel before resolving RGB565. Blending each layer against
// white separately leaves light speckles where the red silhouettes overlap.
template<class C> void cornerIcon(C& c,float x0,float y0,float scale,float press=0){
 auto inside=[](float x,float y,float l,float t,float width,float height,float radius){
  if(x<l||x>l+width||y<t||y>t+height)return false;
  if(x>=l+radius||y>=t+radius)return true;
  const float dx=x-l-radius,dy=y-t-radius;return dx*dx+dy*dy<=radius*radius;
 };
 for(int y=int(std::floor(y0));y<int(std::ceil(y0+56*scale));++y)for(int x=int(std::floor(x0));x<int(std::ceil(x0+68*scale));++x){
  unsigned red=0,green=0,blue=0;
  for(int sy=0;sy<8;++sy)for(int sx=0;sx<8;++sx){
   const float px=(x+(sx+.5f)/8-x0)/scale+52,py=(y+(sy+.5f)/8-y0)/scale+16;
   uint16_t ink=kUiBg;
   // Rounded ends on a narrow quarter-circle button, instead of a second box.
   const float dx=px-100-4*press,dy=py-65-4*press;
   const float angle=std::atan2(dy,dx);
   const float a=std::max(3.14159265f,std::min(4.71238898f,angle<0?angle+6.2831853f:angle));
   const float ex=dx-42*std::cos(a),ey=dy-42*std::sin(a);
   const float distance=std::sqrt(ex*ex+ey*ey);
   if(distance<5)ink=distance<3?kRed:kUiFg;
   if(inside(px,py,65,30,55,42,35)){
    ink=inside(px,py,67,32,51,38,33)?kRed:kUiFg;
    if(inside(px,py,102,54,40,40,14)){
     ink=inside(px,py,104,56,38,38,12)?kUiBg:kUiFg;
    }
   }
   red+=(ink>>11)&31;green+=(ink>>5)&63;blue+=ink&31;
  }
  c.fillRect(x,y,1,1,(((red+32)/64)<<11)|(((green+32)/64)<<5)|((blue+32)/64));
 }
}
template<class C> void buttonDemo(C& c,unsigned ms) {
#if defined(ARDUINO) || defined(ONBOARDING_BAKED_TEST)
 ui::onboarding_baked::shoulder(c,ms);return;
#endif
 const unsigned t=ms%2600;
 const float down=t<500?0:t<1000?ease((t-500)/500.f):t<1500?1:t<1950?1-ease((t-1500)/450.f):0;
 const float press=t<1000?0:t<1140?ease((t-1000)/140.f):t<1380?1:t<1550?1-ease((t-1380)/170.f):0;
 cornerIcon(c,52,16,1,press);
 ui::raster::mask(c,assets::kami_dialogue::kBody,26,10,24,26,kRed,kUiBg);
 ui::raster::mask(c,assets::kami_dialogue::kEyes,30,17,16,10,kUiFg,kRed);
 hand(c,66+4*press,15+14*down+4*press,.52f);
 if(t>=1040&&t<1390){
  Fade<C> f{c,mix(15,0,ease((t-1040)/350.f))};
  f.fillRect(66,9,2,5,kUiFg);f.fillRect(73,17,5,2,kUiFg);
  f.fillRect(55,41,5,2,kUiFg);f.fillRect(56,48,2,4,kUiFg);
 }
}
inline float dialAngle(unsigned ms){
 // Neutral -> -180 -> neutral -> +180 -> neutral, with a pause at each stop.
 const unsigned t=ms%6400,leg=t/1600,within=t%1600;
 const float u=ease((float(within)-200)/1200);
 const float turns=leg==0?-u:leg==1?-1+u:leg==2?u:1-u;
 return 3.14159265f*turns;
}

template<class C> struct Mirror {
 C& target;
 void fillRect(int x,int y,int w,int h,uint16_t color){target.fillRect(280-x-w,y,w,h,color);}
};
template<class C> void dialDemo(C& c,unsigned ms,bool rotate) {
#if defined(ARDUINO) || defined(ONBOARDING_BAKED_TEST)
 if(rotate)ui::onboarding_baked::rotate(c,ms);else ui::onboarding_baked::press(c,ms);return;
#endif
 const unsigned t=ms%(rotate?6400:2600);
 const float reach=t<500?0:t<1000?ease((t-500)/500.f):t<1500?1:t<1950?1-ease((t-1500)/450.f):0;
 const float press=rotate?0:t<1000?0:t<1140?ease((t-1000)/140.f):t<1380?1:t<1550?1-ease((t-1380)/170.f):0;
 const float angle=rotate?dialAngle(ms):0,cx=212-3*press,cy=50;
 ui::drawDialIcon(c,cx,cy,1.f,kUiBg,kRed,angle);
 ui::raster::mask(c,assets::kami_dialogue::kBody,246-mix(0,2,press),37,24,26,kRed,kUiBg);
 ui::raster::mask(c,assets::kami_dialogue::kEyes,250-mix(0,2,press),44,16,10,kUiFg,kRed);
 if(rotate){
  const float grip=-.55f+angle;
  hand(c,cx+21*std::cos(grip),cy+21*std::sin(grip),.5f);
 }else{
  hand(c,239-6*reach-3*press,50,.5f);
  if(t>=1040&&t<1390){
   Fade<C> f{c,mix(15,0,ease((t-1040)/350.f))};
   f.fillRect(232,25,2,5,kUiFg);f.fillRect(239,29,4,2,kUiFg);
   f.fillRect(232,71,2,5,kUiFg);f.fillRect(239,69,4,2,kUiFg);
  }
 }
}
template<class C> void render(C& c,unsigned ms,unsigned page=0) {
 page=std::min(page,6u);
 c.fillRect(0,0,280,240,kUiBg);
#if defined(ARDUINO) || defined(ONBOARDING_BAKED_TEST)
 if(page==0&&ms<kArriveAt){ui::onboarding_baked::entrance(c,ms);return;}
#endif
 const float u=page?1:ease((float(ms)-kMoveAt)/(kArriveAt-kMoveAt));
 const float sway=std::sin(ms*.0032f)*(1-u);
 const float w=106-82*u,h=114-88*u;
 const float entrance=page?0:184*(1-ease(float(ms)/kEntranceMs));
 const float x=87+(kMiniEyeX-87)*u,y=60+(kMiniEyeY-60)*u+2*sway+entrance;
 // Keep the original dialogue frame/font; shorten only its bottom to reserve
 // a clear footer for the new continue prompt.
 if(page||ms>=kArriveAt){
  const int a=page?15:mix(0,15,ease((ms-kArriveAt)/400.f)); Fade<C> f{c,a};
  pixel_dialogue::draw(f,{kDialogueX,kDialogueY,kDialogueW,104},kUiDialogueBorder,kUiDialogueFill);
  speechCorner(f,kDialogueX,kDialogueY);
  typedDialogue(c,ms,page);
 }
 if(page==3){buttonDemo(c,ms);}
 else if(page==4){Mirror<C> mirrored{c};buttonDemo(mirrored,ms);}
 else if(page>=5){dialDemo(c,ms,page==6);}
 else if(page||ms>=kArriveAt){
  ui::raster::mask(c,assets::kami_dialogue::kBody,kMiniEyeX,kMiniEyeY,24,26,kRed,kUiBg);
  const bool blink=ms%4200>4010;
  if(blink){c.fillRect(30,54,6,2,kUiFg);c.fillRect(40,54,6,2,kUiFg);}
  else ui::raster::mask(c,assets::kami_dialogue::kEyes,kMiniEyeX+4,kMiniEyeY+7,16,10,kUiFg,kRed);
 }else{
  mask(c,assets::kBootKamiBodyAlpha4,106,114,x,y,w,h,kRed,kUiBg);
  const float blink=ms>1400&&ms<1580?1-.92f*std::sin((ms-1400)*3.14159265f/180):1;
  const float ew=70-54*u,eh=(46-36*u)*blink;
  mask(c,assets::kBootKamiEyesAlpha4,70,46,x+(w-ew)/2,y+(30-23*u)+(46-36*u-eh)/2,ew,eh,kUiFg,kRed);
 }
 const float wave=std::sin(ms*.011f),scale=1-.66f*u;
 if(page==0)hand(c,x+w+9-3*u,y+30-18*u+wave*(7-5*u),scale);
 if(ms>=readyAt(page)){
  const char* prompts[]={"Press any button","Press any button","Press any button","Press A","Press B","Press C","Turn the C wheel"};
  prompt(c,ms-readyAt(page),prompts[page],"to continue");
 }else if(ms>=textAt(page))prompt(c,ms-textAt(page),"Press B to skip");
}
}}
