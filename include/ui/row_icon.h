#pragma once
#include "ui/proportional_text.h"
// Shared icon painter for onboarding selection rows. Coordinates are outer pixel edges.
namespace ui { namespace row_icon {
template<class C> void icon(C& c,const uint8_t* mask,int w,int h,int x,int rowY,int rowH,uint16_t fg,uint16_t bg){
 auto alpha=[&](int i,int j){if(j<0||j>=h)return 0;unsigned pair=mask[j*((w+1)/2)+i/2];return 17*int(i&1?pair&15:pair>>4);};
 int top=h,bottom=0;for(int j=0;j<h;++j)for(int i=0;i<w;++i)if(alpha(i,j)){top=std::min(top,j);bottom=std::max(bottom,j+1);}
 if(bottom<=top)return;const int y2=2*rowY+rowH-top-bottom,half=y2&1;
 for(int j=0;j<h+half;++j)for(int i=0;i<w;++i){int a=alpha(i,j);if(half)a=(a+alpha(i,j-1)+1)/2;if(a)c.fillRect(x+i,y2/2+j,1,1,proportional::blend(fg,bg,a));}
}
}}
