#pragma once
#include <stdint.h>
namespace apps { namespace onboarding {
// Shared device and Studio lesson: virtual stops keep the marker on the visible arc.
struct Controls {
 static constexpr int kTicksPerRevolution=15;
 static constexpr unsigned kSuccessMs=550;
 unsigned page=3; int ticks=0; bool held=false,success=false;
 int angle=0,undoAngle=0;bool canUndo=false;
 uint32_t successAt=0;
 static bool accepts(unsigned p){return p==3||p==4||p==5||p==6;}
 void reset(unsigned p){page=p;ticks=0;held=success=false;successAt=0;angle=undoAngle=0;canUndo=false;}
 float angleDegrees()const{return float(angle);}
 unsigned next()const{return page==3?4:page==4?6:page==6?5:36;}
 bool complete(uint32_t now)const{return success&&int32_t(now-successAt)>=int32_t(kSuccessMs);}
 void finish(uint32_t now){success=true;held=false;successAt=now;}
 // Return number of semantic scroll-tick haptics (no repeated hold feedback).
 unsigned button(unsigned button,bool down,uint32_t now){
  const unsigned expected=page==3?0:page==4?1:page==5?2:99;
  if(button!=expected||success)return 0;
  if(down){held=true;return 0;}
  if(!held)return 0;
  finish(now);return 1;
 }
 unsigned turn(int delta,uint32_t now,bool undo=false){
  if(page!=6||!delta)return 0;
  if(undo){
   if(canUndo){angle=undoAngle;canUndo=false;if(angle<180)success=false;}
   return 0;
  }
  undoAngle=angle;canUndo=true;
  unsigned moved=0;
  const int direction=delta>0?1:-1;
  for(int i=0;i<(delta>0?delta:-delta);++i){
   int next=angle+direction*(360/kTicksPerRevolution);
   if(next<0)next=0;if(next>180)next=180;
   if(next!=angle){angle=next;ticks+=direction;++moved;}
  }
  // A blocked turn has no stored overshoot and no extra haptic or timer reset.
  if(moved){if(angle==180)finish(now);else success=false;}
  return moved;
 }
};
}}
