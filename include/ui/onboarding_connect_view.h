#pragma once
#include <cmath>
#include "ui/onboarding_copy.h"
#include "ui/onboarding_approved_view.h"
#include "ui/dial_icon.h"
#include "assets/connect_icons.h"
namespace ui { namespace onboarding_connect {
template<class C> void icon(C& c,const uint8_t* mask,int x,int y,uint16_t ink,uint16_t paper){
 for(int row=0;row<24;++row)for(int col=0;col<24;++col)
  c.fillRect(x+col,y+row,1,1,ui::proportional::blend(ink,paper,mask[row*24+col]));
}
template<class C> void selector(C& c,bool audio){
 c.fillRect(0,0,280,240,0xffff);onboarding_wifi::arrows(c,true,true);
 // Existing Kami artwork and wheel silhouette, rasterized at their final size.
 struct Double { C& c;void fillRect(int x,int y,int w,int h,uint16_t ink){c.fillRect(116+x*2,36+y*2,w*2,h*2,ink);} } kami{c};
 ui::raster::mask(kami,assets::kami_dialogue::kBody,0,0,24,26,ui::onboarding_intro::kRed,0xffff);
 ui::raster::mask(kami,assets::kami_dialogue::kEyes,4,7,16,10,0,ui::onboarding_intro::kRed);
 ui::drawDialIcon(c,242,64,.5f,0xffff,ui::onboarding_intro::kRed);
 ui::pixel_dialogue::draw(c,{24,98,232,88},0,0xffff);
 ui::design::paragraph(c,"Use the scroll wheel to select how you'd like to connect.",36,110,208,3);
 for(int i=0;i<2;++i){
  const int x=i?144:24;const bool selected=(i==int(audio));
  const uint16_t paper=selected?0:0xffff,ink=selected?0xffff:0;
  ui::pixel_dialogue::draw(c,{x,194,112,40},0,0xffff);
  if(selected)ui::pixel_dialogue::fillStepped(c,{x+6,200,100,28},2,0);
  ui::design::text(c,i?"Audio":"QR Code",x+10,206,16,ink,paper);
  icon(c,i?assets::connect_icons::audio:assets::connect_icons::qr,x+80,202,ink,paper);
 }
}
inline const char* listeningTitle(bool receiving,const char* error){
 return error&&*error?ui::onboarding_copy::listening_paused:receiving?ui::onboarding_copy::receiving:ui::onboarding_copy::listening;
}
// Approved 1.6-second ear loop: fixed smooth contour, expanding/fading waves.
// Preview-only painter; retain the surrounding canonical dialogue and copy.
template<class C> void listeningEar(C& c,unsigned ms){
 unsigned char coverage[52*48]={};
 auto segment=[&](float ax,float ay,float bx,float by,float width,float opacity){
  const float vx=bx-ax,vy=by-ay,length=vx*vx+vy*vy;
  for(int y=std::max(48,int(std::floor(std::min(ay,by)-width)));y<std::min(96,int(std::ceil(std::max(ay,by)+width)));++y)
   for(int x=std::max(110,int(std::floor(std::min(ax,bx)-width)));x<std::min(162,int(std::ceil(std::max(ax,bx)+width)));++x){
    float t=length?std::max(0.f,std::min(1.f,((x+.5f-ax)*vx+(y+.5f-ay)*vy)/length)):0;
    float a=std::max(0.f,std::min(1.f,width*.5f+.5f-std::hypot(x+.5f-ax-t*vx,y+.5f-ay-t*vy)))*opacity;
    auto& pixel=coverage[(y-48)*52+x-110];pixel=std::max(pixel,static_cast<unsigned char>(a*255));
   }
 };
 auto bezier=[&](float ax,float ay,float bx,float by,float cx,float cy,float dx,float dy,float width){
  float px=ax,py=ay;
  for(int i=1;i<=30;++i){float t=i/30.f,u=1-t,x=u*u*u*ax+3*u*u*t*bx+3*u*t*t*cx+t*t*t*dx,y=u*u*u*ay+3*u*u*t*by+3*u*t*t*cy+t*t*t*dy;segment(px,py,x,y,width,1);px=x;py=y;}
 };
 bezier(138,64,137,48,158,48,158,64,2.5f);
 bezier(158,64,159,73,150,75,149,83,2.5f);
 bezier(149,83,147,94,137,92,139,82,2.5f);
 bezier(143,64,141,56,152,55,152,63,2.2f);
 bezier(152,63,153,68,144,68,145,75,2.2f);
 for(int wave=0;wave<2;++wave){
  float phase=std::fmod((ms%1600)/1600.f+wave*.5f,1.f),r=5+13*phase,opacity=1-(30+210*phase*phase)/255.f;
  float px=132-r*std::cos(-.7f),py=71+r*std::sin(-.7f);
  for(int i=1;i<=40;++i){float a=-.7f+1.4f*i/40,x=132-r*std::cos(a),y=71+r*std::sin(a);segment(px,py,x,y,2,opacity);px=x;py=y;}
 }
 for(int y=0;y<48;++y)for(int x=0;x<52;++x)if(coverage[y*52+x])c.fillRect(110+x,48+y,1,1,ui::proportional::blend(0,0xffff,coverage[y*52+x]));
}
template<class C> void listening(C& c,bool receiving,unsigned parts,unsigned total,const char* error,unsigned ms=0){
 c.fillRect(0,0,280,240,0xffff);onboarding_wifi::arrows(c,true,false);
 ui::design::center(c,listeningTitle(receiving,error),28);
 listeningEar(c,ms);
 ui::pixel_dialogue::draw(c,{24,100,232,96},0,0xffff);
 char message[220];
 if(error&&*error)snprintf(message,sizeof(message),"%s Press A to retry.",error);
 else if(total)snprintf(message,sizeof(message),"Wi-Fi details: %u of %u. Keep playing the sound.",parts,total);
 else snprintf(message,sizeof(message),ui::onboarding_copy::listen_instruction);
 const char* text=message;
 for(int i=0;*text&&i<4;++i){auto line=ui::proportional::line(text,208,18);char row[160];unsigned n=std::min(line.bytes,159u);memcpy(row,text,n);row[n]=0;ui::design::center(c,row,110+i*20,18);if(!line.next)break;text+=line.next;}
 ui::design::instruction(c,ui::onboarding_copy::listen_switch,24,200,232,36);
}
}}
