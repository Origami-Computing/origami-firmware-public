#pragma once
#include "ui/raster.h"
#include "ui/pixel_text.h"
#include "ui/onboarding_intro_view.h"
#include "assets/onboarding_website_qr.h"
#include "qrcodegen.h"
namespace onboarding_wifi {
using namespace ui::onboarding_intro;
constexpr char kWebsite[]="kami.so/device/\nmanage";
inline const char* copy(unsigned page) {
 switch(page){
 case 7:return "Let's connect to\nWi-Fi so you can\ntalk to me.";
 case 8:return "On your phone or\ncomputer go to:\nkami.so/device/\nmanage\nScroll down for\nQR";
 case 9:return "Enter your wifi\n- username:\n- password:\nthen scan to\nconnect";
 case 10:return "Skipping Wi-Fi\nsetup for now.";
 case 11:return "You can connect\nwhenever you want\nfrom the main\nmenu to use\nmy full\ncapabilities.";
 case 16:return "It looks like\nthere was an\nerror. Please try\nagain later.";
 case 17:return "Lastly, let's\nlearn how to\nnavigate around.";
 case 21:return "Congrats, you're\nall done!";
 case 28:return "Are you sure you\nwant to skip this\nstep?";
 case 22:return "If you ever\nforget You can\nalways return to\nthis tutorial in\nsettings";
 default:return "";
 }
}
inline unsigned ready(unsigned page){return 450+kCharacterMs*unsigned(strlen(copy(page)))+140;}

template<class C> void backArrow(C& c,uint16_t ink){
 c.fillRect(25,18,25,3,ink);
 for(int i=0;i<7;++i){c.fillRect(24+i,18-i,3,3,ink);c.fillRect(24+i,18+i,3,3,ink);}
}
template<class C> void arrows(C& c,bool back,bool next,bool skip=false){
 if(back)backArrow(c,kUiFg);
 if(next){Mirror<C> f{c};backArrow(f,kUiFg);}
 if(skip)ui::pixel_text::text(c,"Skip",231,31,1,kUiFg);
}
template<class C> void retryControl(C& c){
 backArrow(c,kUiFg);
 ui::pixel_text::text(c,"Try again",24,32,1,kUiFg);
}

template<class C> void qrIcon(C& c){
 c.fillRect(236,44,29,29,kUiBg);
 for(int y=0;y<21;++y)for(int x=0;x<21;++x){
  bool black=false, finder=false;
  for(int corner=0;corner<3;++corner){int dx=x-(corner==1?14:0),dy=y-(corner==2?14:0);
   if(dx>=0&&dx<7&&dy>=0&&dy<7){finder=true;black=dx==0||dx==6||dy==0||dy==6||(dx>=2&&dx<=4&&dy>=2&&dy<=4);}
  }
  if(!finder)black=(x>7||y>7)&&((x*3+y*5+x*y)%7<3);
  if(black)c.fillRect(240+x,48+y,1,1,kUiFg);
 }
}
template<class C> void footer(C& c,unsigned age,const char* first,const char* second=""){
 prompt(c,age,first,second);
}
inline bool hasBack(unsigned page){return page!=21&&page!=22&&page!=28;}
constexpr unsigned kWebsiteScrollMax=7;
inline int websiteShift(unsigned scroll){return int(scroll==999?0:std::min(scroll,kWebsiteScrollMax))*22;}
// Independent from the text cache so the cue continues pulsing after typing.
template<class C> void websiteArrow(C& c,unsigned ms,int shift){
 if(shift || ms<ready(8))return;
 const unsigned phase=(ms-ready(8))%2000;
 const unsigned alpha=phase<=1000?phase*15/1000:(2000-phase)*15/1000;
 const uint16_t ink=ui::raster::blend(kUiFg,kUiDialogueFill,alpha);
 c.fillRect(84,201,30,28,kUiDialogueFill);
 c.fillRect(87,204,16,3,ink);c.fillRect(100,204,3,18,ink);
 for(int i=0;i<7;++i){c.fillRect(94+i,215+i,3,3,ink);c.fillRect(106-i,215+i,3,3,ink);}
}
template<class C> void websiteContent(C& c,unsigned ms,unsigned scroll,int shiftOverride=-1){
 const unsigned position=scroll==999?0:std::min(scroll,kWebsiteScrollMax);
 // Seven wheel steps retain the exact full-QR endpoint, with intermediate views.
 const int shift=shiftOverride<0?websiteShift(position):shiftOverride;
 struct Clip {
  C& c;
  void fillRect(int x,int y,int w,int h,uint16_t ink){
   const int l=std::max(x,34),t=std::max(y,86),r=std::min(x+w,242),b=std::min(y+h,234);
   if(r>l&&b>t)c.fillRect(l,t,r-l,b-t,ink);
  }
 } content{c};
 textViewport(content,copy(8),ms<450?0:(ms-450)/kCharacterMs,0,38,88-shift,6);
 // The entire 29-module code plus four-module white border fits at 4px/module.
 // Draw exact modules, without bitmap scaling or antialiasing QR edges.
 content.fillRect(64,240-shift,148,148,kUiBg);
 if(core::release_policy::stagingBuild){
  uint8_t temporary[qrcodegen_BUFFER_LEN_FOR_VERSION(6)],qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
  if(qrcodegen_encodeText(assets::onboarding_website::kUrl,temporary,qr,qrcodegen_Ecc_LOW,2,6,qrcodegen_Mask_AUTO,true)){
   const int size=qrcodegen_getSize(qr),module=148/(size+8),x0=(280-size*module)/2;
   for(int y=0;y<size;++y)for(int x=0;x<size;++x)if(qrcodegen_getModule(qr,x,y))content.fillRect(x0+module*x,256+module*y-shift,module,module,kUiFg);
  }
 }else for(unsigned y=0;y<29;++y)for(unsigned x=0;x<29;++x)
  if(assets::onboarding_website::kRows[y]&(1u<<(28-x)))content.fillRect(80+4*x,256+4*y-shift,4,4,kUiFg);
 c.fillRect(246,92,2,128,kUiMuted);
 c.fillRect(245,92+shift*110/154,4,18,kUiFg);
 websiteArrow(c,ms,shift);
}

template<class C> void dialogue(C& c,unsigned page,unsigned ms,unsigned scroll=999,bool automaticScanner=false,int websiteShiftOverride=-1){
 c.fillRect(0,0,280,240,kUiBg);
 ui::pixel_dialogue::draw(c,{kDialogueX,kDialogueY,kDialogueW,(page==8||page==9)?162:104},kUiDialogueBorder,kUiDialogueFill);
 speechCorner(c,kDialogueX,kDialogueY);
 ui::raster::mask(c,assets::kami_dialogue::kBody,kMiniEyeX,kMiniEyeY,24,26,kRed,kUiBg);
 ui::raster::mask(c,assets::kami_dialogue::kEyes,kMiniEyeX+4,kMiniEyeY+7,16,10,kUiFg,kRed);
 if(page!=28)arrows(c,hasBack(page),true,page==9&&!automaticScanner);
 if(page==9&&!automaticScanner)qrIcon(c);
 if(page==8){websiteContent(c,ms,scroll,websiteShiftOverride);return;}
 textViewport(c,copy(page),ms<450?0:(ms-450)/kCharacterMs,scroll,38,88,page==9?6:4);
 if(page==9&&automaticScanner)return;
 if(ms<ready(page)){
  if(ms>=450)footer(c,ms-450,"Press B to skip");
 }else if(page==9&&!automaticScanner)footer(c,ms-ready(page),"Press C to scan");
 else if(page==28)footer(c,ms-ready(page),"A: No / B: Yes");
 // Arrow navigation is self-contained; no duplicate continue/back footer.
}
}
