#pragma once
#include <algorithm>
#include <cmath>
#include <stdint.h>

namespace ui { namespace rounded_fill {
// Native-pixel RGB565 coverage. No enlarged bitmap, allocation, panel reads or
// second framebuffer. Keep fractional geometry until coverage is evaluated.
struct Shape { float x, y, w, h, radius; };
struct Clip { int x, y, w, h; };
inline uint16_t blend(uint16_t ink, uint16_t paper, unsigned a) {
    const unsigned b=255-a;
    return uint16_t((((((ink>>11)&31)*a+((paper>>11)&31)*b+127)/255)<<11) |
                    (((((ink>>5)&63)*a+((paper>>5)&63)*b+127)/255)<<5) |
                    (((ink&31)*a+(paper&31)*b+127)/255));
}
inline unsigned coverage(const Shape& s, int x, int y) {
    if (s.w<=0 || s.h<=0) return 0;
    const float r=std::max(0.f,std::min(s.radius,std::min(s.w,s.h)*.5f));
    const float dx=std::abs(x+.5f-(s.x+s.w*.5f))-(s.w*.5f-r);
    const float dy=std::abs(y+.5f-(s.y+s.h*.5f))-(s.h*.5f-r);
    // Signed distance gives a one-native-pixel coverage ramp at the boundary.
    // Straight edges and interiors avoid sqrt; only rounded corners need it.
    float distance=std::max(dx,dy)-r;
    if(dx>0 && dy>0) distance=std::sqrt(dx*dx+dy*dy)-r;
    return unsigned(std::max(0.f,std::min(1.f,.5f-distance))*255.f+.5f);
}
// Caller supplies the actual flat background and a clip in the surface's local
// coordinates. Paint full-coverage spans together; only edge pixels are blended.
template<class C> void draw(C& c,const Shape& s,uint16_t ink,uint16_t paper,Clip clip) {
    if(s.w<=0 || s.h<=0 || clip.w<=0 || clip.h<=0) return;
    const int left=std::max(clip.x,int(std::floor(s.x)));
    const int right=std::min(clip.x+clip.w,int(std::ceil(s.x+s.w)));
    const int top=std::max(clip.y,int(std::floor(s.y)));
    const int bottom=std::min(clip.y+clip.h,int(std::ceil(s.y+s.h)));
    // The shape is convex, so each row's full-coverage pixels are one span:
    // evaluate coverage only from each end inward to that span.
    for(int y=top;y<bottom;++y) {
        int x=left;
        for(;x<right;++x) {
            const unsigned a=coverage(s,x,y);
            if(a==255) break;
            if(a)c.fillRect(x,y,1,1,blend(ink,paper,a));
        }
        if(x==right) continue;
        int last=right-1;
        for(;last>x;--last) {
            const unsigned a=coverage(s,last,y);
            if(a==255) break;
            if(a)c.fillRect(last,y,1,1,blend(ink,paper,a));
        }
        c.fillRect(x,y,last-x+1,1,ink);
    }
}
} } // namespace ui::rounded_fill
