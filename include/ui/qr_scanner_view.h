#pragma once
#include "ui/onboarding_copy.h"
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
  c.fillRect(0,0,280,36,0xffff);
  ui::design::center(c,ui::onboarding_copy::scanner_title,12,18);
  c.fillRect(0,200,280,40,0xffff);
  ui::design::instruction(c,ui::onboarding_copy::scanner_switch,24,200,232,36);
}
template<class C> void appOverlay(C& c){
 overlay(c);
 c.fillRect(0,0,280,35,0xffff);ui::design::center(c,"Hold steady over app QR",14,18);
 c.fillRect(0,194,280,46,0xffff);
 ui::design::choice(c,"A back",24,194,112,30,false);ui::design::choice(c,"B skip",144,194,112,30,false);
}
// Small regions preserve the scanner's incremental direct-panel updates.
template<class F> void regions(F emit) {
 emit(60,40,21,21);emit(200,40,21,21);
 emit(60,180,21,21);emit(200,180,21,21);
 emit(0,0,280,36);emit(0,200,280,40);
}
}
