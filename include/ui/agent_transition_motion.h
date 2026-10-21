#pragma once
#include <algorithm>
namespace ui { namespace agent_motion {
// Millisecond sampling is independent of rendering speed. Preview first;
// firmware state-machine integration follows visual approval.
inline float ease(float x){x=std::max(0.f,std::min(1.f,x));return x*x*(3-2*x);}
inline float ramp(unsigned ms,unsigned start,unsigned length){return ease((float(ms)-start)/length);}
struct Frame {
 float avatar=0,body=0,menu=0,glow=0,listening=0,dots=0,bubble=0,eyes=1;
 float offsetX=0,offsetY=0;
 unsigned letters=0;
};
inline Frame menuHome(unsigned ms){Frame f;f.menu=1-ramp(ms,100,200);f.eyes=ramp(ms,300,300);return f;}
// Acknowledge capture on the first frame; decorative movement must not hide it.
inline Frame homeListen(unsigned ms){Frame f;f.glow=1;f.listening=1;return f;}
inline Frame listenReply(unsigned ms){Frame f;f.avatar=f.body=1;f.listening=1-ramp(ms,150,330);f.glow=1-ramp(ms,150,550);f.dots=ramp(ms,440,180)*(1-ramp(ms,960,200));f.bubble=ramp(ms,1030,420);f.letters=ms>1450?(ms-1450)/56:0;return f;}
constexpr unsigned kReplyHomeDurationMs=520;
inline Frame replyHome(unsigned ms){
 Frame f;f.bubble=1-ramp(ms,0,120);f.body=1-ramp(ms,40,90);f.letters=200;
 const float t=std::max(0.f,std::min(1.f,(float(ms)-40)/480));
 const float q=t-1;
 // Fast departure, gentle positional overshoot, then a zero-velocity settle.
 // Clamp size at the canonical home shape: momentum moves the whole face only.
 const float travel=t>=1?0:-(1.9f*q*q*q+.9f*q*q);
 f.avatar=std::max(0.f,std::min(1.f,travel));
 f.offsetX=-102*(travel-f.avatar);f.offsetY=-75*(travel-f.avatar);
 return f;
}
inline unsigned duration(unsigned clip){return clip==1?1500:clip==2?2300:clip==3?4800:clip==4?1700:11800;}
inline Frame sample(unsigned clip,unsigned ms){
 if(clip==1)return menuHome(ms);
 if(clip==2)return homeListen(ms);
 if(clip==3)return listenReply(ms);
 if(clip==4)return replyHome(ms);
 if(ms<1700)return menuHome(ms);
 if(ms<4400)return homeListen(ms-1700);
 if(ms<9700)return listenReply(ms-4400);
 return replyHome(ms-9700);
}
}}
