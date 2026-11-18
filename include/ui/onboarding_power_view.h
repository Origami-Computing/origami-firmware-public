#pragma once
#include "ui/eye_rig.h"
#include "ui/onboarding_flow_view.h"
namespace ui { namespace onboarding_power {
inline float spring(unsigned ms){return 1-std::exp(-float(ms)/110)*std::cos(float(ms)/95);}
template<class C> void render(C& c,unsigned page,unsigned ms){
 c.fillRect(0,0,280,240,0xffff);
  const float push=page==161?spring(ms):page==162?1-spring(ms):0;
  const float gaze=page==160?5*std::sin(ms*.0008f):0;
  const float ox=-32*push+gaze,oy=page==160?3*std::sin(ms*.0011f):0;
  float closure=1;
  if(page==160){const unsigned blink=ms%4800;if(blink>4200){const auto f=ui::kami_expression::blink(blink-4200);closure=f.shape==ui::kami_expression::Shape::Squeeze?.1f:f.squash;}}
  ui::eye_rig::RectSurface<C> eyes{c,0xffff};
  ui::eye_rig::drawRaster(eyes,18,43,244,134,ui::eye_rig::kPresets[0].pose,closure,0xffff,78,ox,oy,false,255,0,-1,ui::eye_rig::kHomeSpacing,1);
  // Preserve the wheel asset and move it inward; the eyes move away faster.
  struct Wheel {C& s;int dx;unsigned alpha;
   void fillRect(int x,int y,int w,int h,uint16_t color){
    x+=dx;y-=20;
    const int l=std::max(0,x),r=std::min(280,x+w),t=std::max(0,y),b=std::min(240,y+h);
    if(r>l&&b>t)s.fillRect(l,t,r-l,b-t,ui::proportional::blend(color,0xffff,alpha));
   }
  } wheel{c,int(std::lround(26-24*push)),page==160?unsigned(110+145*(.5f+.5f*std::cos(ms*.00349f))):255u};
  ui::onboarding_flow::wheel(wheel,90,false,0xffff);
  ui::design::center(c,"Hold C for 2 seconds and release",193,14);
  ui::design::center(c,"to turn me on/off",212,14);
 ui::onboarding_flow::promptArrows(c,ms,true,false);
}
}}
