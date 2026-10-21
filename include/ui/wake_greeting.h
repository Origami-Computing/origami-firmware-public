#pragma once
#include "ui/eye_rig_math.h"
namespace ui { namespace wake_greeting {
constexpr uint32_t kDuration=2180;
struct Frame { float face, x, handX, handY, handAlpha; };
inline float smooth(float t){t=eye_rig::clamp(t,0,1);return t*t*(3-2*t);}
inline Frame sample(uint32_t age){
  const float ms=(float)age-420;
  float p=0,swing=0;
  if(ms>0&&ms<340){float q=ms/340-1,c=1.70158f*1.1f;p=1+(c+1)*q*q*q+c*q*q;}
  else if(ms>=340&&ms<1380){p=1;swing=sinf((ms-340)/260*2*3.14159265f)*17*3.14159265f/180;}
  else if(ms>=1380&&ms<1760){float t=(ms-1380)/380;p=1-(t<.5f?2*t*t:1-(-2*t+2)*(-2*t+2)/2);}
  float expression=smooth(ms/280)*(1-smooth((ms-1380)/380));
  float face=age<300?.5f*(1-smooth(age/300.f)):expression;
  return {face,-16*p,114.4f+(-36.96f+sinf(swing)*145.2f)*p,
          // Keep the hand below the firmware's taller 38px status strip.
          52.8f+(-110.f+145.2f*(1-cosf(swing)))*p,
          smooth(ms/140)*(1-smooth((ms-1380)/380))};
}
}}
