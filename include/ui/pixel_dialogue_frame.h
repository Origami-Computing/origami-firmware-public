#pragma once
#include <stdint.h>
#include <algorithm>
#include "ui/raster.h"
#include "config/ui_theme.h"
#include "ui/safe_area_layout.h"

namespace ui { namespace pixel_dialogue {
constexpr int kStroke=2;
// A stepped silhouette keeps the small corners crisp on the 280x240 panel.
template<class Surface>
void fillStepped(Surface& dst, safe_area::Box b, int cut, uint16_t ink) {
  dst.fillRect(b.x,b.y+cut,b.w,b.h-2*cut,ink);
  for(int row=0;row<cut;row+=kStroke) {
    const int inset=cut-row;
    dst.fillRect(b.x+inset,b.y+row,b.w-2*inset,kStroke,ink);
    dst.fillRect(b.x+inset,b.y+b.h-row-kStroke,b.w-2*inset,kStroke,ink);
  }
}
template<class Surface>
void draw(Surface& dst, safe_area::Box b, uint16_t ink, uint16_t paper) {
  if(b.w<20 || b.h<20) return;
  dst.fillRect(b.x,b.y,b.w,b.h,paper);
  fillStepped(dst,b,6,ink);
  fillStepped(dst,{b.x+2,b.y+2,b.w-4,b.h-4},4,paper);
  fillStepped(dst,{b.x+4,b.y+4,b.w-8,b.h-8},4,ink);
  fillStepped(dst,{b.x+6,b.y+6,b.w-12,b.h-12},2,paper);
  // Tiny square corner accents echo the stepped rails without pictorial motifs.
  for(int side=0;side<2;++side) for(int end=0;end<2;++end)
    dst.fillRect(b.x+(side?b.w-4:2),b.y+(end?b.h-4:2),kStroke,kStroke,ink);
}
// Shared with the tutorial: a double-rule pointer to the upper-left Kami.
template<class C> void speechCorner(C& c,int x,int y,uint16_t border=kUiDialogueBorder,uint16_t paper=kUiDialogueFill) {
 // Offset every rail perpendicular to the SAME diagonal (slope 1/2).
 // Two-pixel ink / two-pixel paper / two-pixel ink, including the miter.
 // Native-pixel coverage avoids stair-step changes in apparent stroke weight.
 constexpr float normalLength=1.11803398875f;
 for(int row=0;row<22;++row) {
  int start=0;uint16_t last=paper;
  for(int col=0;col<=28;++col) {
   int coverage=0;
   if(col<28)for(int sy=0;sy<4;++sy)for(int sx=0;sx<4;++sx) {
    const float px=col+(sx+.5f)/4,py=row+(sy+.5f)/4;
    const float diagonal=(py-.5f*px)/normalLength;
    const float inset=std::min(px,std::max(py-10,diagonal));
    if((inset>=0&&inset<2)||(inset>=4&&inset<6))++coverage;
   }
   const uint16_t ink=ui::raster::blend(border,paper,(coverage*15+8)/16);
   if(col>0&&(ink!=last||col==28)) {c.fillRect(x+start,y-10+row,col-start,1,last);start=col;}
   last=ink;
  }
 }
}
}}
