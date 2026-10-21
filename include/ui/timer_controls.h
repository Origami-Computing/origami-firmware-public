#pragma once
#include <math.h>
#include <time.h>
#include <stdio.h>
#include "config/ui_theme.h"

namespace ui { namespace timer_controls {
constexpr uint16_t kActive=0xFA89; // approved coral #ff5149
constexpr uint16_t kInactive=0xBDF7;
constexpr uint16_t kLavender=0xC5BF; // #c4b5fd, quantized to RGB565
template<class C> void back(C& c) {
 c.fillRect(25,22,25,3,kUiFg);
 for(int i=0;i<7;++i){c.fillRect(24+i,22-i,3,3,kUiFg);c.fillRect(24+i,22+i,3,3,kUiFg);}
}
template<class C> void stop(C& c,int x,int y,bool active){c.fillRect(x,y,16,16,active?kActive:kInactive);}
template<class C> void playback(C& c,bool running) {
 if(running){c.fillRect(241,56,5,16,kUiFg);c.fillRect(251,56,5,16,kUiFg);}
 else for(int i=0;i<8;++i)c.fillRect(242+i,56+i,1,16-i*2,kUiFg);
}
template<class C> void clock(C& c,uint16_t bg=kUiBg) {
 time_t now=time(nullptr);struct tm local{};localtime_r(&now,&local);char value[8];strftime(value,sizeof(value),"%H:%M",&local);
 c.setTextFont(1);c.setTextSize(2);c.setTextColor(kUiFg,bg);c.setTextDatum(4);c.drawString(value,140,24);c.setTextDatum(0);
}
template<class C> void snooze(C& c) {
 const int marks[][4]={{237,66,10,10},{248,59,7,7},{257,53,5,5}};
 for(const auto& m:marks){c.drawLine(m[0],m[1],m[0]+m[2],m[1],kUiFg);c.drawLine(m[0]+m[2],m[1],m[0],m[1]+m[3],kUiFg);c.drawLine(m[0],m[1]+m[3],m[0]+m[2],m[1]+m[3],kUiFg);}
}
struct RingPose {float pair,local;};
inline RingPose ringingPose(uint32_t ms,int eye) {
 // Bound the phase so long-running alerts retain precision and millis wrap is harmless.
 const float t=ms%28000u,phase=t/280.f*6.283185307f,lag=.48f+eye*.05f;
 float after=0;const int reversal=(int)floorf((t-70)/140);
 for(int j=0;j<4;++j){const int n=reversal-j;const float age=(t-(70+n*140))/1000.f;
  if(age>=0)after+=(n%2==0?1:-1)*.75f*expf(-age*25)*sinf(age*6.283185307f*11);
 }
 return {5*sinf(phase),1.8f*(sinf(phase-lag)-sinf(phase))+after};
}
template<class C> void homePill(C& c,uint32_t seconds) {
 c.fillRoundRect(100,40,80,25,12,kUiFg);
 char value[16];snprintf(value,sizeof(value),"%02lu:%02lu",(unsigned long)(seconds/60),(unsigned long)(seconds%60));
 c.setTextFont(1);c.setTextSize(2);c.setTextColor(kLavender,kUiFg);c.setTextDatum(4);c.drawString(value,140,53);c.setTextDatum(0);
}
} }
