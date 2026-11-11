#pragma once
#ifdef ESP32
#include <esp_heap_caps.h>
#include "ui/onboarding_control_rasters.h"
#endif
#include "studio_dial_icon.h"
#include "studio_usb_animation.h"
#include "studio_copy_animation.h"
#include "kami_nod/kami_nod_settings.h"
#define STUDIO_KAMI_NOD 1
#include "ui/onboarding_copy.h"
#include "ui/onboarding_approved_view.h"
#include "studio_qr.h"
// Shared device/host composition for the November branching onboarding flow.
#include "ui/onboarding_proposals.h"
namespace ui { namespace onboarding_flow {
template<class C> void pill(C& c,int y,const char* text,bool selected,int height=36){
 ui::design::choice(c,text,24,y,232,height,selected);
}
// Imported Kami Nod renderer/preset at a caller-selected native body width.
// A fresh readable tile provides correct alpha compositing on every frame.
// UI-task-only reusable scratch, shared by nod and home-eye compositing.
// Keep large raster buffers out of internal DRAM and the UI task stack.
#ifdef ESP32
inline void* rasterScratch(){static void* p=heap_caps_calloc(190*130,1,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);return p;}
#else
inline void* rasterScratch(){static unsigned char p[190*130];return p;}
#endif
template<class C> void nod(C& c,int x,int y,unsigned ms,int bodyWidth=60,uint16_t bg=0xffff){
 struct Tile {
  uint16_t* pixels;
  Tile(uint16_t* buffer):pixels(buffer){}
  uint16_t at(int x,int y)const{return pixels[y*80+x];}
  void fillRect(int x,int y,int w,int h,uint16_t ink){for(int yy=y;yy<y+h;++yy)for(int xx=x;xx<x+w;++xx)pixels[yy*80+xx]=ink;}
 };auto* buffer=static_cast<uint16_t*>(rasterScratch());if(!buffer)return;Tile tile(buffer);std::fill(tile.pixels,tile.pixels+80*88,bg);
 auto preset=kami_nod::saved;preset.sizePercent=(bodyWidth*1000+120)/240;
 kami_nod::render(tile,80,88,10,8,ms,preset);
 for(int yy=0;yy<88;++yy)for(int xx=0;xx<80;++xx)
  if(tile.at(xx,yy)!=bg)c.fillRect(x-10+xx,y-8+yy,1,1,tile.at(xx,yy));
}
template<class C> void hand(C& c,unsigned ms){
 const float travel=(1-std::cos((ms%1800)*6.28318530718f/1800))*.5f;
 const float cx=241+travel*12,cy=66-travel*12,r=4;
 for(int y=49;y<72;++y)for(int x=235;x<260;++x){
  unsigned samples=0;
  for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx){const float dx=x+(sx+.5f)/4-cx,dy=y+(sy+.5f)/4-cy;if(dx*dx+dy*dy<r*r)++samples;}
  if(samples)c.fillRect(x,y,1,1,ui::proportional::blend(0xf945,0xffff,samples*255/16));
 }
}
template<class C> void speaking(C& c,unsigned page,unsigned ms){
 c.fillRect(0,0,280,240,0xffff);onboarding_wifi::arrows(c,page==22,true);
 nod(c,110,24,ms);
 const char* text=page==21?ui::onboarding_copy::nav_congrats:ui::onboarding_copy::nav_reminder;
 char lines[5][160]{};int count=0;
 while(*text&&count<5){auto line=ui::proportional::line(text,232,18);unsigned n=std::min(line.bytes,159u);memcpy(lines[count++],text,n);if(!line.next)break;text+=line.next;}
 const int first=144-(count-1)*12;
 for(int i=0;i<count;++i)ui::design::center(c,lines[i],first+i*24);
}
template<class C> void welcome(C& c,unsigned ms){
 c.fillRect(0,0,280,240,0xffff);
 nod(c,110,20,ms);
 ui::design::center(c,ui::onboarding_copy::welcome_title,104);
 // Alpha is 0..255: a 2.8-second fade from readable gray to white and back.
 const auto ink=ui::proportional::blend(0,0xffff,unsigned((1+cos(ms*.002244))*64));
 ui::design::center(c,ui::onboarding_copy::welcome_prompt_1,170,18,ink);
 ui::design::center(c,ui::onboarding_copy::welcome_prompt_2,194,18,ink);
}
// Reuse the original 28-nub wheel's vector painter at its final size. Its
// 4x4 subpixel coverage is computed at native resolution, not bitmap-upscaled.
// Clip the part beyond the physical screen and leave the background untouched.
template<class C> void wheel(C& c,float degrees,bool held,uint16_t bg){
 degrees=std::max(0.f,std::min(180.f,degrees));
#ifdef ESP32
 const unsigned step=unsigned(degrees)/12;
 if(degrees==float(step*12)&&((bg==0xffff&&(!held||degrees==0))||(bg==0x364a&&!held&&(degrees==0||degrees==180)))){
  const unsigned frame=bg==0x364a?(degrees==180?18:17):held?16:step;
  ui::onboarding_rasters::draw(c,frame);return;
 }
#endif
 struct Clip {C& target;uint16_t bg;void fillRect(int x,int y,int w,int h,uint16_t ink){
  if(ink==bg)return;
  const int left=std::max(0,x),top=std::max(0,y);
  const int right=std::min(280,x+w),bottom=std::min(240,y+h);
  if(right>left&&bottom>top)target.fillRect(left,top,right-left,bottom-top,ink);
 }} clipped{c,bg};
 ui::drawStudioDialIcon(clipped,held?232.f:274.f,132.f,58.f/17.65f,bg,0xf945,
                  -1.57079632679f-degrees*.01745329252f);
}
// Shoulder geometry follows the approved UI illustration; the original-size
// Kami Nod character follows its ready/held pose. B mirrors this composition.
template<class C> void shoulderA(C& c,bool held,uint16_t bg,unsigned ms,bool drawKami=true){
#ifdef ESP32
 if(bg==0xffff||(!held&&bg==0x364a)){
  ui::onboarding_rasters::draw(c,bg==0x364a?21:held?20:19);
  if(drawKami)nod(c,held?86:76,held?85:75,ms,46,bg);return;
 }
#endif
 const auto sample=[&](float x,float y)->uint16_t {
  const uint16_t rim=0x6000,red=0xf945,shade=0xd104,light=0xfaeb;
  uint16_t ink=bg;
  const float dx=x-236,dy=y-216,r=std::sqrt(dx*dx+dy*dy);
  if(x<=236&&y<=216&&r<108){
   ink=r>104?rim:r>99?light:red;
   if(r<48)ink=bg;else if(r<52)ink=rim;
  }
  // Rounded arc cap follows the actual curved shoulder, not a top tab.
  const float shift=held?6.f:0.f;
  const float bx=x-236-shift,by=y-216-shift;
  float a=std::atan2(by,bx);a=std::max(-2.99f,std::min(-1.73f,a));
  const float ex=bx-115*std::cos(a),ey=by-115*std::sin(a);
  const float distance=std::sqrt(ex*ex+ey*ey);
  if(distance<8)ink=distance>5.5f?rim:red;
  if(distance<4&&std::sqrt(bx*bx+by*by)>116)ink=light;
  // Same circular hand used throughout onboarding, now on the contact pose.
  const float hx=x-(held?151.f:135.f),hy=y-(held?137.f:113.f);
  const float hr=std::sqrt(hx*hx+hy*hy);
  if(hr<10)ink=hr>8.5f?shade:red;
  return ink;
 };
 for(int y=84;y<217;++y){
  int run=-1;uint16_t previous=bg;
  for(int x=96;x<=237;++x){
   unsigned rr=0,gg=0,bb=0;
   for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx){
    const uint16_t p=sample(x+(sx+.5f)/4,y+(sy+.5f)/4);
    rr+=p>>11;gg+=(p>>5)&63;bb+=p&31;
   }
   const uint16_t ink=uint16_t(((rr+8)/16)<<11|((gg+8)/16)<<5|(bb+8)/16);
   if(run>=0&&(ink!=previous||x==237)){c.fillRect(run,y,x-run,1,previous);run=-1;}
   if(x<237&&ink!=bg&&run<0)run=x;
   previous=ink;
  }
 }
 if(drawKami)nod(c,held?86:76,held?85:75,ms,46,bg);
}
template<class C> void control(C& c,unsigned page,unsigned ms,float degrees=0,bool held=false,bool success=false){
 const uint16_t bg=success?0x364a:0xffff;
 c.fillRect(0,0,280,240,bg);
 ui::design::center(c,page==3?ui::onboarding_copy::press_a:page==4?ui::onboarding_copy::press_b:page==6?ui::onboarding_copy::spin_c:ui::onboarding_copy::press_c,24,24,0,bg);
 if(page==5||page==6){nod(c,110,99,ms,60,bg);wheel(c,degrees,held,bg);
 ui::onboarding_proposals::wheelHand(c,page==6,ms,held,success,bg);
 return;}
 if(page==3){shoulderA(c,held,bg,ms);return;}
 struct Mirrored {C& target;void fillRect(int x,int y,int w,int h,uint16_t ink){
  target.fillRect(280-x-w,y,w,h,ink);
 }} mirrored{c};
 shoulderA(mirrored,held,bg,ms);
}

template<class C> void selection(C& c,unsigned selected){
 c.fillRect(0,0,280,240,0xffff);
 ui::design::center(c,ui::onboarding_copy::select_title_1,28);ui::design::center(c,ui::onboarding_copy::select_title_2,52);
 const char* labels[]={"Select the bottom row",ui::onboarding_copy::select_continue};
 const uint8_t* icons[]={nullptr,nullptr};
 ui::onboarding_proposals::rows(c,labels,icons,2,selected,92,42,true);
}
template<class C> void menu(C& c,unsigned page,unsigned selected){
 c.fillRect(0,0,280,240,0xffff);
 if(page==37)ui::design::center(c,ui::onboarding_copy::interest_title,24);else onboarding_wifi::arrows(c,true,false);
 if(page==39){ui::design::center(c,ui::onboarding_copy::wifi_question_1,16);ui::design::center(c,ui::onboarding_copy::wifi_question_2,40);}
 const char* interest[]={ui::onboarding_copy::interest_build,ui::onboarding_copy::interest_document,ui::onboarding_copy::interest_both,ui::onboarding_copy::interest_restart};
 const char* account[]={"Create account/login","Continue as guest"};
 const char* wifi[]={ui::onboarding_copy::wifi_join,ui::onboarding_copy::wifi_skip};
 const uint8_t* icons[]={assets::menu_icons::kCode,assets::menu_icons::kCamera,ui::onboarding_proposals::kBoth,ui::onboarding_proposals::kRestart};
 const uint8_t* wifiIcons[]={assets::menu_icons::kWifi,ui::onboarding_proposals::kSkip};
 ui::onboarding_proposals::rows(c,page==37?interest:page==38?account:wifi,page==37?icons:wifiIcons,page==37?4:2,selected,page==37?56:88);
 if(page==38){ui::design::center(c,"Use account to sync",168,16,kUiMuted);ui::design::center(c,"media online",190,16,kUiMuted);}
}
// Consistent title / QR / URL zones. QR matrices retain four quiet modules.
template<class C> void qr(C& c,unsigned page,bool staging=true){
 using namespace studio_qr;
 const bool store=page==128;
 c.fillRect(0,0,280,240,0xffff);onboarding_wifi::arrows(c,true,true);
 ui::design::center(c,store?"Published Apps":page==40?ui::onboarding_copy::login_title:page==41?ui::onboarding_copy::wifi_page_title:ui::onboarding_copy::projects_title,16,18);
 const auto& link=(staging?staging_links:production_links)[store?3:page-40];
 const int size=link.size,scale=4,x0=(280-size*scale)/2,y0=36;
 for(int y=0;y<size;++y)for(int x=0;x<size;++x)if(link.modules[y*size+x])c.fillRect(x0+x*scale,y0+y*scale,scale,scale,0);
 int footerSize=16;
 while(footerSize>12 && std::max(ui::proportional::width(link.line1,footerSize),ui::proportional::width(link.line2,footerSize))>248)footerSize-=2;
 ui::design::center(c,link.line1,link.line2[0]?200:208,footerSize);
 if(link.line2[0])ui::design::center(c,link.line2,218,footerSize);
}

// Shared by the firmware and the host preview; all copy is visible immediately.
template<class C> void build(C& c,unsigned page,unsigned ms=0,bool connected=false){
 const uint16_t bg=page==47&&connected?0x364a:0xffff;
 c.fillRect(0,0,280,240,bg);onboarding_wifi::arrows(c,true,true);
 if(page==45||page==49){
  int kamiX=110;
  if(page==45){
   int textWidth=0;
   for(const char* line:{ui::onboarding_copy::build_intro_1,ui::onboarding_copy::build_intro_2,ui::onboarding_copy::build_intro_3})
    textWidth=std::max(textWidth,ui::proportional::width(line,ui::design::standard));
   kamiX=140+(textWidth+1)/2-60;
  }
  nod(c,kamiX,44,ms);
  if(page==45){
   struct HandPosition {C& c;int dx;void fillRect(int x,int y,int w,int h,uint16_t ink){c.fillRect(x+dx,y,w,h,ink);}} positionedHand{c,kamiX-180};
   hand(positionedHand,ms);
   ui::design::center(c,"To build a project,",128);
   ui::design::center(c,"follow these three steps.",152);
  }else{
   ui::design::center(c,ui::onboarding_copy::build_done_1,128);
   ui::design::center(c,ui::onboarding_copy::build_done_2,152);
   ui::design::center(c,ui::onboarding_copy::build_done_3,176);
  }
  return;
 }
 if(page==47){
  ui::design::text(c,"Skip",197,14,16,0,bg);
 }
 const unsigned step=page==47?1:page==48?2:3;
 struct LargeNumber{C& c;void fillRect(int x,int y,int w,int h,uint16_t ink){c.fillRect(x*2,y*2+44,w*2,h*2,ink);}} number{c};
 if(page!=47
 &&page!=48
 )ui::design::center(number,step==1?"01.":step==2?"02.":"03.",0,18,0,0xffff,70);
 if(page==47){
  ui::design::center(c,"01.",12,24,0,bg);
  studio_usb::draw(c,connected?1800:ms);
  ui::design::center(c,ui::onboarding_copy::connect_device,166,18,0,bg);
  ui::design::center(c,ui::onboarding_copy::connect_computer,186,16,0,bg);
 }
 else if(page==48){
  ui::design::center(c,"02.",12,24);
  const char* line="Browse to the following link";
  const int kamiX=140+(ui::proportional::width(line,ui::design::standard)+1)/2-60;
  nod(c,kamiX,44,ms);
  struct HandPosition {C& c;int dx;void fillRect(int x,int y,int w,int h,uint16_t ink){c.fillRect(x+dx,y,w,h,ink);}} positionedHand{c,kamiX-180};hand(positionedHand,ms);
  ui::design::center(c,line,144);
 }
 else{
  if(page==46||page==48)studio_copy::draw(c,ms);
  else ui::design::instruction(c,ui::onboarding_copy::copy_prompt,40,96,200,36);
  ui::design::center(c,page==46?ui::onboarding_copy::build_setup:ui::onboarding_copy::build_prompt,148);
  ui::design::center(c,ui::onboarding_copy::coding_agent,172);
 }
 for(unsigned i=0;i<3;++i){
  const int x=62+i*56;const bool done=i+1<step;const uint16_t paper=done?0x2104:0xdedb;
  // Rounded capsules rasterized at native resolution with 4x coverage.
  for(int yy=0;yy<22;++yy)for(int xx=0;xx<44;++xx){
   unsigned coverage=0;for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx){
    float px=xx+(sx+.5f)/4,py=yy+(sy+.5f)/4;
    float dx=px-std::max(11.f,std::min(33.f,px)),dy=py-11;
    if(dx*dx+dy*dy<=121)++coverage;
   }
   if(coverage)c.fillRect(x+xx,204+yy,1,1,ui::proportional::blend(paper,bg,coverage*255/16));
  }
  const char* label=i==0?"1":i==1?"2":"3";
  ui::design::center(c,label,207,16,done?0xffff:0,paper,x+22);
  if(i+1==step)c.fillRect(x+16,229,12,2,0);
 }

}
template<class C> void wifiError(C& c,const char* title,const char* detail,bool audio,unsigned selected){
 c.fillRect(0,0,280,240,0xffff);onboarding_wifi::arrows(c,true,false);
 ui::design::center(c,title,48,18);
 ui::design::paragraph(c,detail,24,88,232,3);
 const char* labels[]={ui::onboarding_copy::try_again,audio?ui::onboarding_copy::try_qr:ui::onboarding_copy::try_audio};
 const uint8_t* icons[]={ui::onboarding_proposals::kRestart,audio?assets::menu_icons::kCamera:assets::menu_icons::kVolume};
 ui::onboarding_proposals::rows(c,labels,icons,2,selected,148,36);
}
}}
