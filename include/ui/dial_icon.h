#pragma once
#include <cmath>
#include <cstdint>

namespace ui {
// Meet Kami's wheel silhouette, shared with smaller in-game prompts.
template<class C> void drawDialIcon(C& c, float cx, float cy, float scale,
                                   uint16_t bg, uint16_t red = 0xF945,
                                   float angle = 0) {
  const int extent = int(std::ceil(20 * scale));
  for (int y=int(cy)-extent; y<int(cy)+extent; ++y)
    for (int x=int(cx)-extent; x<int(cx)+extent; ++x) {
      unsigned rsum=0, gsum=0, bsum=0;
      for (int sy=0; sy<4; ++sy) for (int sx=0; sx<4; ++sx) {
        const float dx=(x+(sx+.5f)/4-cx)/scale;
        const float dy=(y+(sy+.5f)/4-cy)/scale;
        const float r=std::sqrt(dx*dx+dy*dy), a=std::atan2(dy,dx)-angle;
        const float radius=17.f+.65f*(1+std::cos(28*a))*.5f;
        uint16_t ink=bg;
        if (r<radius) {
          ink=r>radius-.9f?0xC084:red;
          if (r<2.4f) ink=0x6000;
          if (r>10 && r<14 && std::cos(a)>.97f && std::abs(std::sin(a))<.19f) ink=bg;
        }
        rsum+=(ink>>11)&31; gsum+=(ink>>5)&63; bsum+=ink&31;
      }
      c.fillRect(x,y,1,1,(((rsum+8)/16)<<11)|(((gsum+8)/16)<<5)|((bsum+8)/16));
    }
}
}
