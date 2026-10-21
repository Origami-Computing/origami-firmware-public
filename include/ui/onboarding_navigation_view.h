#pragma once
#include "ui/raster.h"
#include "ui/pixel_text.h"
#include "ui/onboarding_dialogue_view.h"
namespace onboarding_navigation {
using namespace ui::onboarding_intro;
template<class C> void bubble(C& c,int y,int h,bool centered=false){
 ui::pixel_dialogue::draw(c,{18,(int16_t)y,244,(int16_t)h},kUiDialogueBorder,kUiDialogueFill);
 if(!centered){speechCorner(c,18,y);return;}
 // Two parallel rails form a centered tail pointing toward Kami's eyes.
 for(int py=y-14;py<y+7;++py)for(int px=121;px<160;++px){
  int hits=0;
  for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx){
   const float x=px+(sx+.5f)/4,yy=py+(sy+.5f)/4;
   const float top=std::min(float(y),float(y-12)+std::abs(x-140)*.8f);
   const float d=(yy-top)/(top<y?1.28062485f:1.f);
   if((d>=0&&d<2)||(d>=4&&d<6))++hits;
  }
  c.fillRect(px,py,1,1,ui::raster::blend(kUiFg,kUiBg,(hits*15+8)/16));
 }
}
template<class C> void pointingHand(C& c,unsigned ms){
 const float reach=(1-std::cos((ms%2200)*6.2831853f/2200))/2;
 hand(c,253+4*reach,112,1.64f);
}
inline const char* talkCopy(unsigned page){
 return page==24?"Great, there's one\nlast thing.":page==25?"Hold A + B from\nthe home screen\nto talk to me\nany time.":"Hold A + B\nto talk to me";
}
inline unsigned talkReady(unsigned page){return 450+unsigned(strlen(talkCopy(page)))*kCharacterMs+140;}
template<class C> void smallControl(C& c,int y=190){
#if defined(ARDUINO) || defined(ONBOARDING_BAKED_TEST)
 struct Shift {C& target;int dy;void fillRect(int x,int yy,int w,int h,uint16_t ink){target.fillRect(x,yy+dy,w,h,ink);}} shifted{c,y-146};
 ui::onboarding_baked::control(shifted,0);
#else
 cornerIcon(c,30,y,.53f);
#endif
}
template<class C> void shoulder(C& c,bool right,int y=190){
 if(right){Mirror<C> mirror{c};smallControl(mirror,y);}else smallControl(c,y);
}

template<class C> void instruction(C& c,bool back,bool home=false){
 bubble(c,178,54,home);
 if(home){ui::pixel_text::centered(c,"Rotate scroll wheel",188,2);ui::pixel_text::centered(c,"to go to main menu",208,2);}
 else if(back){shoulder(c,false);shoulder(c,true);ui::pixel_text::centered(c,"Press A + B",188,2);ui::pixel_text::centered(c,"to go back",208,2);}
 else {ui::pixel_text::centered(c,"Rotate C to browse",188,2);ui::pixel_text::centered(c,"C enter / A back",208,2);}
}
template<class C> void talkInstruction(C& c,unsigned page,unsigned ms,unsigned scroll=999){
 const bool explanation=page==25;const int y=explanation?140:178;
 bubble(c,y,explanation?94:54,true);
 if(explanation){
  // Exact menu-size icons in a fixed row above the scrolling text viewport.
  shoulder(c,false,146);shoulder(c,true,146);
 }
 textViewport(c,talkCopy(page),ms<450?0:(ms-450)/kCharacterMs,scroll,38,explanation?182:y+10,2,true);
 onboarding_wifi::arrows(c,explanation,!explanation||ms<talkReady(page));
}
}
