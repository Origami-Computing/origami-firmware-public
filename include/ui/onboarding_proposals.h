#pragma once
// Approved onboarding visuals shared by Studio and device.
#include "ui/list_view.h"
#include "ui/row_ink_alignment.h"
#include "ui/row_icon.h"
#include "ui/scene.h"
#include "assets/menu_icons.h"
#include "ui/onboarding_proposal_icons.h"
namespace ui { namespace onboarding_proposals {
template<class C> void dot(C& c,float cx,float cy,float radius,uint16_t color,uint16_t bg){
 for(int y=int(cy-radius)-1;y<=int(cy+radius)+1;++y)for(int x=int(cx-radius)-1;x<=int(cx+radius)+1;++x){
  const float a=std::max(0.f,std::min(1.f,radius+.5f-std::hypot(x+.5f-cx,y+.5f-cy)));
  if(a)c.fillRect(x,y,1,1,ui::proportional::blend(color,bg,unsigned(a*255)));
 }
}
template<class C> void wheelHand(C& c,bool spin,unsigned ms,bool held,bool success,uint16_t bg){
 if(spin){
  if(success)return;
  const unsigned phase=ms%1800;
  const float direction=(ms%3600)<1800?-1.f:1.f;
  const float travel=std::max(0.f,std::min(1.f,(float(phase)-250.f)/1100.f));
  const float angle=direction*travel*travel*(3.f-2.f*travel)*1.57079632679f;
  const float alpha=phase<250?phase/250.f:phase<1350?1.f:phase<1650?(1650-phase)/300.f:0.f;
  dot(c,274-72*std::cos(angle),132+72*std::sin(angle),5,ui::proportional::blend(0xf945,bg,unsigned(alpha*255)),bg);
 }else{
  const float travel=(1-std::cos(ms*6.2831853f/1400))*.5f;
  dot(c,held?172.f:180.f+travel*14,132,5,0xf945,bg);
 }
}
template<class C> void rows(C& c,const char* const* labels,const uint8_t* const* icons,unsigned count,unsigned selected,int top,int pitch=40,bool firstDown=false){
 // Use the approved proportional painter on both targets, rather than the
 // generic device Scene label renderer (whose font metrics differ from Studio).
 for(unsigned i=0;i<count;++i){
  const auto box=ui::safe_area::fitBox({16,top+int(i)*pitch,249,36});
  const uint16_t bg=i==selected?0:0xffff,fg=i==selected?0xffff:0;
  for(int y=0;y<box.h;++y)for(int x=0;x<box.w;++x){
   const int dx=x<5?4-x:x>=box.w-5?x-(box.w-5):0;
   const int dy=y<5?4-y:y>=box.h-5?y-(box.h-5):0;
   if(dx*dx+dy*dy<=25)c.fillRect(box.x+x,box.y+y,1,1,bg);
  }
  ui::ListItem item;item.label=labels[i];item.icon=icons[i];item.fg=0;
  const auto row=ui::presentListRow(item,i==selected,box.x,box.y,box.w,36,0,0xffff,0xffff);
  if(row.icon)ui::row_icon::icon(c,row.icon,row.iconSize,row.iconSize,row.iconX,box.y,box.h,fg,bg);
  const char* label=i==selected?item.label:row.label;
  auto centeredY=[&](const char* text){return ui::row_ink::textY(text,18,box.y,box.h);};
  ui::design::text(c,label,row.textX,centeredY(label),18,fg,bg);
  if(!(firstDown&&i==0))ui::design::text(c,">",row.indicatorX,centeredY(">"),18,fg,bg);
 }
 if(firstDown){const uint16_t bg=selected==0?0:0xffff,fg=selected==0?0xffff:0;
  for(int y=0;y<20;++y)for(int x=0;x<20;++x){int i=y*20+x;unsigned alpha=(kDown[i/2]>>(i%2?0:4))&15;if(alpha)c.fillRect(240+x,top+8+y,1,1,ui::proportional::blend(fg,bg,alpha*17));}
 }
}
}}
