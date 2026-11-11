#pragma once
#include "ui/proportional_text.h"
#include "ui/pixel_dialogue_frame.h"
#include "ui/raster.h"
#include "assets/kami_dialogue.h"
#include "config/display_safe_area.h"
// R7 review candidate. Not a global theme override until approved.
namespace ui { namespace design {
constexpr int margin=24, width=232, standard=18, line=24, footerY=194, footerH=30;
// Stable baseline for every line: uppercase ascent, not the individual string.
template<class C> void text(C& c,const char* value,int x,int y,int size=standard,uint16_t ink=0,uint16_t paper=0xffff){
 proportional::atBaseline(c,value,x,y-proportional::top("Ag",size),size,ink,paper);
}
template<class C> void center(C& c,const char* v,int y,int size=standard,uint16_t ink=0,uint16_t paper=0xffff,int cx=140){text(c,v,cx-proportional::width(v,size)/2,y,size,ink,paper);}
template<class C> void paragraph(C& c,const char* v,int x,int y,int w,int rows=4){for(int i=0;*v&&i<rows;++i){auto l=proportional::line(v,w,standard);char s[160];unsigned n=std::min(l.bytes,159u);memcpy(s,v,n);s[n]=0;text(c,s,x,y+i*line);if(!l.next)break;v+=l.next;}}
template<class C> void instruction(C& c,const char* v,int x=24,int y=footerY,int w=width,int h=footerH){pixel_dialogue::draw(c,{x,y,w,h},0,0xffff);const int textY=y+(h-proportional::height(v,standard))/2+proportional::top("Ag",standard)-proportional::top(v,standard);center(c,v,textY,standard,0,0xffff,x+w/2);}
template<class C> void actions(C& c,const char* left,const char* right){if(left)instruction(c,left,24,footerY,112);if(right)instruction(c,right,144,footerY,112);}

// Kami Double Frame: canonical outlined choice, including inverse focus.
template<class C> void choice(C& c,const char* value,int x,int y,int w,int h,bool selected){
 const uint16_t paper=selected?0:0xffff,ink=selected?0xffff:0;
 pixel_dialogue::draw(c,{x,y,w,h},0,0xffff);
 if(selected)pixel_dialogue::fillStepped(c,{x+6,y+6,w-12,h-12},2,0);
 const int ty=y+(h-proportional::height(value,standard))/2+proportional::top("Ag",standard)-proportional::top(value,standard);
 center(c,value,ty,standard,ink,paper,x+w/2);
}
template<class C> void response(C& c,const char* value,int rows=3){raster::mask(c,assets::kami_dialogue::kBody,28,46,24,26,0xf945,0xffff);raster::mask(c,assets::kami_dialogue::kEyes,32,53,16,10,0,0xf945);pixel_dialogue::draw(c,{24,82,232,98},0,0xffff);pixel_dialogue::speechCorner(c,24,82);paragraph(c,value,36,96,208,rows);}
// Same outline/inset/fill mechanics as the OTA update bar, width is caller-owned.
template<class C> void progress(C& c,int x,int y,int w,int percent){percent=std::max(0,std::min(100,percent));c.fillRect(x,y,w,20,0);c.fillRect(x+2,y+2,w-4,16,0xffff);if(percent)c.fillRect(x+3,y+3,(w-6)*percent/100,14,0);}
template<class C> void row(C& c,const char* label,const char* value,int y,bool selected,int height=28){c.fillRect(24,y,232,height,selected?0:0xffff);text(c,label,30,y+6,18,selected?0xffff:0,selected?0:0xffff);if(value)text(c,value,151,y+6,18,selected?0xffff:0,selected?0:0xffff);}
}}
