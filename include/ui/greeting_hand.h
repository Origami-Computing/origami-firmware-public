#pragma once
#include "ui/wake_greeting.h"
#include "config/ui_layout.h"
namespace ui { namespace greeting_hand {
template<class C>void draw(C& t,const wake_greeting::Frame& f,int cx,int cy){
 namespace rig=eye_rig;
  // Coverage-render the white hand and soft outline at subpixel positions.
  if(f.handAlpha>0){
    float hx=cx+f.handX,hy=cy+f.handY;
    for(int yy=(int)floorf(hy-16);yy<=(int)ceilf(hy+17);++yy)
      for(int xx=(int)floorf(hx-16);xx<=(int)ceilf(hx+16);++xx){
        float dx=xx+.5f-hx,dy=yy+.5f-hy;
        float d=sqrtf(dx*dx+dy*dy),shadow=sqrtf(dx*dx+(dy-1.5f)*(dy-1.5f));
        float shade=rig::clamp((17-shadow)/3,0,1)*.23f*f.handAlpha*(1-rig::clamp(14-d,0,1));
        if(shade>0&&xx>=0&&xx<t.width()&&yy>=kSystemStatusBarH&&yy<t.height()){
          uint8_t v=(uint8_t)(255*(1-shade));t.drawPixel(xx,yy,t.color565(v,v,v));
        }
      }
  }
}
}}
