#pragma once
// Approved Onboarding Studio composition, shared by the device and host preview.
#include <string>
#include "ui/onboarding_intro_view.h"
#include "ui/onboarding_dialogue_view.h"
#include "ui/onboarding_navigation_view.h"
#include "ui/proposals/design_system.h"
#include "assets/onboarding_animations.h"
#include "assets/dialogue_controls.h"
#include "qrcodegen.h"
#include "ui/eye_rig_raster.h"
namespace ui { namespace onboarding_approved {
using namespace ui::onboarding_intro;
inline const char* copy(unsigned p){
 switch(p){
 case 0:return "Hi, I'm Kami...";
 case 1:return "I'm your personal digital sidekick, like Bmo or Jarvis...";
 case 2:return "There's a lot that I can do, so let me give you a tour.";
 case 3:return "This is my A button. Press it to continue.";
 case 4:return "This is my B button. Press it to continue.";
 case 5:return "This is my C wheel. Press it to continue.";
 case 6:return "You can also rotate the C wheel. Give it a try to continue.";
 case 7:return "Now lets connect me to your account";
 case 9:return "When you're ready press B to scan the WiFi QR code and connect your account.";
 case 17:return "Great! Now I'll show you how to navigate on this device.";
 case 18:return "To open my menu rotate the scroll wheel";
 case 21:return "Congrats, you're all done!";
 case 22:case 34:return "Remember if youre every confused, you can always return to this tutorial in Menu > Settings > Redo Tutorial";
 case 32:return "Follow the onboarding instructions on web, until you've entered your wifi information";
 case 33:return "This is my home screen. Here you can talk to me, record audio, take photos, or open my menu";
 case 24:return "Great, there's one last thing.";
 case 25:return "While speaking hold A+B down together to talk to me";
 case 29:return "Great! Now press B to open the camera.";
 default:return onboarding_wifi::copy(p);
 }
}
// HAPTICS + SCROLL + UI / R7 shared candidates, imported unchanged for preview.
constexpr int kBottom=224,kTextPad=16,kTextX=36,kTextWidth=208;
inline unsigned textRows(int top){return unsigned(std::max(1,1+(kBottom-top-2*kTextPad-ui::proportional::height("Ag",18))/ui::design::line));}
// Measure the actual 18 px proportional glyph advances; never approximate columns.
inline std::string wrap(const char* source){
 std::string out;
 while(*source){auto l=ui::proportional::line(source,kTextWidth,18);if(!l.next)break;out.append(source,l.bytes);source+=l.next;if(*source)out+='\n';}
 return out;
}
template<class C> void draftText(C& c,const std::string& source,unsigned count,unsigned scroll,int top,bool centered=false){
 const unsigned rows=textRows(top),total=lineCount(source.c_str()),limit=total>rows?total-rows:0;
 scroll=std::min(scroll,limit);unsigned line=0,offset=0;
 for(size_t start=0;start<source.size();++line){
  size_t end=source.find('\n',start);if(end==std::string::npos)end=source.size();
  const unsigned shown=count>offset?std::min(unsigned(end-start),count-offset):0;
  if(line>=scroll&&line<scroll+rows){const std::string value=source.substr(start,shown);ui::design::text(c,value.c_str(),(centered?140-ui::proportional::width(source.substr(start,end-start).c_str(),18)/2:kTextX),top+kTextPad+(line-scroll)*ui::design::line);}
  offset+=unsigned(end-start)+1;start=end+1;
 }
 if(limit){const int h=rows*ui::design::line-6;c.fillRect(248,top+kTextPad,2,h,kUiMuted);c.fillRect(247,top+kTextPad+int(scroll)*(h-16)/int(limit),4,16,kUiFg);}
}
template<class C> void setupQr(C& c,unsigned ms,const char* label,bool back,bool next){
 c.fillRect(0,0,280,240,kUiBg);
 onboarding_wifi::arrows(c,back,next);
 // Use the exact instruction fade/breathing timing, translated to the header.
 const float opacity=ms<kPromptFadeMs?(11.f/15)*ease(float(ms)/kPromptFadeMs):(8.5f+2.5f*std::cos((ms-kPromptFadeMs)*6.2831853/2600))/15;
 const int g5=int(std::lround(31*(1-opacity))),g6=int(std::lround(63*(1-opacity)));
 ui::design::center(c,label,12,18,(g5<<11)|(g6<<5)|g5);
 // Same URL, compact version-2 encoding through the repository's QR encoder.
 // 125 px symbol retains crisp 5 px modules and the full 20 px quiet zone.
 uint8_t temporary[qrcodegen_BUFFER_LEN_FOR_VERSION(6)],qr[qrcodegen_BUFFER_LEN_FOR_VERSION(6)];
 if(!qrcodegen_encodeText(assets::onboarding_website::kUrl,temporary,qr,qrcodegen_Ecc_LOW,2,6,qrcodegen_Mask_AUTO,true))return;
 const int size=qrcodegen_getSize(qr);
 const int module=core::release_policy::stagingBuild?165/(size+8):5;
 const int x0=(280-size*module)/2,y0=core::release_policy::stagingBuild?(230-size*module)/2:52;
 for(int y=0;y<qrcodegen_getSize(qr);++y)for(int x=0;x<qrcodegen_getSize(qr);++x)
  if(qrcodegen_getModule(qr,x,y))c.fillRect(x0+module*x,y0+module*y,module,module,kUiFg);
 if(core::release_policy::stagingBuild){
  // The longer staging hostname needs two standard-font lines. Keep a full
  // four-module quiet zone between the code and either readable fallback line.
  ui::design::center(c,KAMI_ONBOARDING_HOST_LABEL,198,12);
  ui::design::center(c,"/device/manage",214);
 }else ui::design::center(c,"kami.so/device/manage",204);
}

template<class C> void photoReviewChrome(C& c){
 struct Outline{C& c;int dx,dy;void fillRect(int x,int y,int w,int h,uint16_t){c.fillRect(x+dx,y+dy,w,h,kUiBg);}};
 for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){Outline o{c,dx,dy};onboarding_wifi::arrows(o,true,true);}
 onboarding_wifi::arrows(c,true,true);ui::design::actions(c,"A retake","B send");
}

// One menu-practice prompt, reused in menus and entered apps.
template<class C> void menuInstruction(C& c,bool practiced){
 if(!practiced){ui::design::instruction(c,"C to select");return;}
 ui::pixel_dialogue::draw(c,{24,164,232,60},0,0xffff);
 for(int y=0;y<19;++y)for(int x=0;x<24;++x){auto l=assets::dialogue_controls::left[y*24+x],r=assets::dialogue_controls::right[y*24+x];if(l!=0xffff)c.fillRect(37+x,171+y,1,1,l);if(r!=0xffff)c.fillRect(219+x,171+y,1,1,r);}
 ui::design::center(c,"A+B together",175);
 ui::design::center(c,"to go back",199);
}

// Home-control lessons keep the approved home-eye composition from Haptics R7.
constexpr unsigned kWheelFrames=36;
inline unsigned wheelFrame(unsigned ms){return (ms%2600)*kWheelFrames/2600;}
inline float wheelAngle(unsigned frame){return float(frame)*6.2831853f/kWheelFrames;}
template<class C>void wheelText(C& c,unsigned ms){
 struct Shift{C& c;void fillRect(int x,int y,int w,int h,uint16_t ink){c.fillRect(x,y+39,w,h,ink);}} shifted{c};
 draftText(shifted,"To open my menu\nrotate the scroll wheel",ms<450?0:(ms-450)/56,0,120,true);
}
template<class C> void navigation(C& c,unsigned page,unsigned ms,unsigned selection,unsigned menu=0,bool practiced=false,const char* app="Camera"){
 c.fillRect(0,0,280,240,kUiBg);
 if(page==25||page==26){
  struct Shift{C& c;void fillRect(int x,int y,int w,int h,uint16_t ink){c.fillRect(x,y-12,w,h,ink);}} shifted{c};
  ui::onboarding_baked::talkEyes(shifted,18000);
 }else ui::onboarding_baked::eyes(c,18000);
 if(page==24)onboarding_wifi::arrows(c,false,true);
 if(page==25||page==26)onboarding_wifi::arrows(c,true,false);
 if(page==18){
  ui::drawDialIcon(c,254,112,.6f,kUiBg,kRed,wheelAngle(wheelFrame(ms)));
  ui::pixel_dialogue::draw(c,{24,164,232,60},0,0xffff);
  wheelText(c,ms);
 }else if(page==24){
  ui::pixel_dialogue::draw(c,{24,164,232,60},0,0xffff);
  struct Shift{C& c;void fillRect(int x,int y,int w,int h,uint16_t ink){c.fillRect(x,y+39,w,h,ink);}} shifted{c};
  draftText(shifted,wrap(copy(24)),ms<450?0:(ms-450)/56,0,120,true);
 }else{
  ui::pixel_dialogue::draw(c,{24,118,232,106},0,0xffff);
  for(int y=0;y<19;++y)for(int x=0;x<24;++x){auto l=assets::dialogue_controls::left[y*24+x],r=assets::dialogue_controls::right[y*24+x];if(l!=0xffff)c.fillRect(37+x,129+y,1,1,l);if(r!=0xffff)c.fillRect(219+x,129+y,1,1,r);}
  const std::string value=wrap(copy(25));
  struct Shift{C& c;void fillRect(int x,int y,int w,int h,uint16_t ink){c.fillRect(x,y+18,w,h,ink);}} shifted{c};
  draftText(shifted,value,ms<450?0:(ms-450)/56,selection,120,true);
 }
}

template<class C> void background(C& c,unsigned page,unsigned ms,int height,int top,bool back,bool next){
 c.fillRect(0,0,280,240,kUiBg);
 onboarding_wifi::arrows(c,back,next);
 if(page>=3&&page<=6){
  struct Move{C& c;int dx;void fillRect(int x,int y,int w,int h,uint16_t ink){c.fillRect(x+dx,y+16,w,h,ink);}} moved{c,page<5?70:-92};
  if(page==3)ui::onboarding_baked::shoulder(moved,ms);
  else if(page==4){Mirror<C> mirrored{c};struct Shift{Mirror<C>& c;void fillRect(int x,int y,int w,int h,uint16_t ink){c.fillRect(x+70,y+16,w,h,ink);}} shifted{mirrored};ui::onboarding_baked::shoulder(shifted,ms);}
  else if(page==5)ui::onboarding_baked::press(moved,ms);
  else ui::onboarding_baked::rotate(moved,ms);
 }else if(page==33){
  // Rasterize normal home geometry directly at its displayed size. Preserve
  // subpixel coverage instead of dropping rows/columns from a baked bitmap.
  struct HomeEyes{
   C& c;bool swap;
   explicit HomeEyes(C& target):c(target),swap(false){}
   bool getSwapBytes(){return swap;}void setSwapBytes(bool v){swap=v;}
   uint16_t readPixel(int,int){return kUiBg;}
   void pushImage(int x,int y,int w,int h,const uint16_t* data){
    for(int row=0;row<h;++row)for(int col=0;col<w;){
     const auto raw=data[row*w+col];int end=col+1;while(end<w&&data[row*w+end]==raw)++end;
     c.fillRect(x+col,y+row,end-col,1,swap?raw:uint16_t((raw>>8)|(raw<<8)));col=end;
    }
   }
  } eyes{c};
  ui::eye_rig::drawRaster(eyes,80,32,120,80,ui::eye_rig::kPresets[0].pose,
                        1,kUiBg,52.8f,0,0,false,255,0,-1,ui::eye_rig::kHomeSpacing,1);
 }else{
 // Reduce the original 106x114 boot masks, never enlarge the tiny dialogue icon.
 const float h=float(height),w=h*106/114,x=(280-w)/2,y=std::max(24.f,(top-h)/2.f);
 mask(c,assets::kBootKamiBodyAlpha4,106,114,x,y,w,h,kRed,kUiBg);
 const float eh=h*46/114,ew=w*70/106;
 if(ms%4200>4010){c.fillRect(int(x+w*.22f),int(y+h*.43f),int(w*.22f),2,kUiFg);c.fillRect(int(x+w*.57f),int(y+h*.43f),int(w*.22f),2,kUiFg);}
 else mask(c,assets::kBootKamiEyesAlpha4,70,46,x+(w-ew)/2,y+h*30/114,ew,eh,kUiFg,kRed);
 }
 // Exact production double-rule frame, native 2x firmware font, and scroll painter.
 ui::pixel_dialogue::draw(c,{24,(int16_t)top,232,(int16_t)(kBottom-top)},kUiDialogueBorder,kUiDialogueFill);
}
template<class C> void proposal(C& c,unsigned page,unsigned ms,unsigned scroll,int height,int top,const char* text,bool back,bool next){
 background(c,page,ms,height,top,back,next);
 draftText(c,wrap(text),ms<450?0:(ms-450)/kCharacterMs,scroll,top,true);
}
}}
