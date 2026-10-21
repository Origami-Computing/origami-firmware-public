#pragma once
#include "ui/camera_photographer_view.h"
#include "ui/onboarding_dialogue_view.h"

namespace qr_scanner_view {
// Canvas supplies fillRect and at, so Kami blends against the camera image.
template<class C> void overlay(C& c) {
  // Only white corner brackets; camera pixels between them stay untouched.
  const int x=60,y=40,size=160;
  for(int side=0;side<2;++side)for(int bottom=0;bottom<2;++bottom){
   const int cx=side?x+size-20:x,cy=bottom?y+size-2:y;
   const int vx=side?x+size-2:x,vy=bottom?y+size-20:y;
   c.fillRect(cx,cy,21,3,0xFFFF);c.fillRect(vx,vy,3,21,0xFFFF);
  }
  // Exact onboarding B-arrow anchor, with a one-pixel black outline.
  struct ArrowLayer {
   C& c;int dx,dy;uint16_t ink;
   void fillRect(int x,int y,int w,int h,uint16_t){c.fillRect(280-x-w+dx,y+dy,w,h,ink);}
  };
  for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){ArrowLayer a{c,dx,dy,0};onboarding_wifi::backArrow(a,0);}
  ArrowLayer a{c,0,0,0xFFFF};onboarding_wifi::backArrow(a,0xFFFF);
  camera_photographer::photographer(c,0);
  c.fillRect(56,207,200,19,0);
  const char* hint="Place QR code inside frame";
  ui::pixel_text::text(c,hint,56+(200-int(strlen(hint))*6)/2,213,1,0xFFFF);
}
// Small regions preserve the scanner's incremental direct-panel updates.
template<class F> void regions(F emit) {
 emit(60,40,21,21);emit(200,40,21,21);
 emit(60,180,21,21);emit(200,180,21,21);
 emit(227,10,31,20);emit(24,188,24,28);emit(56,207,200,19);
}
}
