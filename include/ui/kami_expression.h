#pragma once
#include "ui/eye_rig_math.h"
#include "ui/wake_greeting.h"

// Pure, elapsed-time web rig motions. Shared by hardware and host checks.
namespace ui { namespace kami_expression {
enum class Shape : uint8_t { Open, Squeeze, Bowl, Arc, Spiral };
enum class Action : uint8_t { None, Blink, Sleep, Wake, Startle, Smile, Dizzy };
struct Frame {
  Shape shape=Shape::Open;
  float squash=1, pop=1, spinMs=0;
  int burstMs=-1, zzzMs=-1;
  bool neutral=false;
  int wakeMs=-1;
  float startleMix=-1, recoilX=0, recoilY=0; // recoil in nominal eye widths
};
inline const char* name(Action a) {
  static const char* names[]={"none","blink","sleep","wake","startle","smile","dizzy"};
  return names[(unsigned)a];
}
inline bool action(const char* s,Action& out) {
  for(unsigned i=0;i<7;++i)if(s&&!strcmp(s,name((Action)i))){out=(Action)i;return true;}
  return false;
}
inline bool namedShape(const char* s,Shape& shape) {
  if(!s)return false;
  if(!strcmp(s,"sleeping"))shape=Shape::Bowl;
  else if(!strcmp(s,"closed_smile")||!strcmp(s,"delighted_smile"))shape=Shape::Arc;
  else if(!strcmp(s,"dizzy"))shape=Shape::Spiral;
  else if(!strcmp(s,"squeezed"))shape=Shape::Squeeze;
  else return false;
  return true;
}
inline const char* shapeName(Shape s) {
  static const char* names[]={"open","squeeze","bowl","arc","spiral"};return names[(unsigned)s];
}
inline float backOut(float p,float strength) {
  if(p>=1)return 1;float q=eye_rig::clamp(p,0,1)-1,c=1.70158f*strength;
  return 1+(c+1)*q*q*q+c*q*q;
}
inline Frame blink(uint32_t ms,uint32_t hold=0) {
  Frame f;
  if(ms<50)f.squash=1-.9f*(ms/50.f)*(ms/50.f);
  else if(ms<130+hold)f.shape=Shape::Squeeze;
  else if(ms<250+hold)f.squash=.1f+.9f*backOut((ms-130-hold)/120.f,1);
  return f;
}
// Map a held blink into the cached 250 ms timeline without reopening during
// its closed hold. The frame stores both shape and squash.
inline uint32_t blinkCacheMs(uint32_t age,uint32_t hold=0) {
  if(age>=130u+hold)age-=hold;
  else if(age>=130u)age=129;
  return age>250?250:age;
}
// Approved direct-to-sleep preview: original 520 ms gentle closing speed.
constexpr uint32_t kSleepCloseMs=520;
constexpr uint32_t kSleepPoseHoldMs=800;
inline float sleepClose(uint32_t ms) {
  float p=eye_rig::clamp(ms/(float)kSleepCloseMs,0,1);
  p=p<.5f?2*p*p:1-2*(1-p)*(1-p);
  return 1-.9f*p;
}
inline uint32_t duration(Action a,uint32_t hold=0) {
  switch(a){case Action::Blink:return 250+hold;case Action::Sleep:return kSleepCloseMs+3400;
    case Action::Wake:return wake_greeting::kDuration;case Action::Startle:return 780;
    case Action::Smile:return 1700;case Action::Dizzy:return 3100+hold;default:return 0;}
}
inline Frame sample(Action a,uint32_t ms,uint32_t hold=0) {
  Frame f;
  if(a==Action::Blink)return blink(ms,hold);
  if(a==Action::Sleep){f.neutral=true;if(ms<kSleepCloseMs)f.squash=sleepClose(ms);else{f.shape=Shape::Bowl;f.zzzMs=ms-kSleepCloseMs;}return f;}

  if(a==Action::Startle){
    // Approved Bump D: symmetric snap, astonished hold, neutral recovery.
    f.neutral=true;f.startleMix=0;
    if(ms<55){float q=1-ms/55.f;f.startleMix=1-q*q*q;f.squash=1+.08f*f.startleMix;}
    else if(ms<115){float q=1-(ms-55)/60.f;f.startleMix=1;f.squash=1+.08f*q*q*q;}
    else if(ms<400)f.startleMix=1;
    else if(ms<780){float p=(ms-400)/380.f;f.startleMix=1-p*p*(3-2*p);}
    return f;
  }
  if(a==Action::Wake){f.neutral=true;f.wakeMs=ms;return f;}

  if(a==Action::Smile||a==Action::Dizzy){
    const Shape target=a==Action::Smile?Shape::Arc:Shape::Spiral;
    uint32_t stay=a==Action::Smile?1200:2600+hold;
    if(ms<250){f=blink(ms);if(ms>=130)f.shape=target;}
    else if(ms<250+stay)f.shape=target;
    else {uint32_t end=ms-250-stay;f=blink(end);if(end<50)f.shape=target;}
    if(f.shape==Shape::Spiral)f.spinMs=ms>130?ms-130:0;
  }return f;
}

inline float softSpring(uint32_t ms){
  if(!ms||ms>=780)return 0;
  // D3 preview curve, normalized at its first peak (91.23ms).
  const float peak=atanf(11.f/7.f)/11.f;
  const float norm=expf(-7*peak)*sinf(11*peak);
  float fade=eye_rig::clamp((ms-620.f)/160.f,0,1);
  // COM14 feedback: carry more of the surprise in translation (+35% travel).
  return (23.f/48.f)*expf(-7.f*ms/1000.f)*sinf(11.f*ms/1000.f)/norm*(1-fade*fade*(3-2*fade));
}

// Acceleration delta rejects static gravity and gradual tilting. Four distinct
// jolts in 900ms become a shake. Poll on UI core; no gyro or wake flags needed.
class MotionDetector {
  int32_t previous_[3]={};
  uint32_t sampleAt_=0,pulseAt_=0,windowAt_=0;
  unsigned pulses_=0;
  bool ready_=false;
  float directionX_=0,directionY_=0;
public:
  float directionX()const{return directionX_;}
  float directionY()const{return directionY_;}
  void reset(){ready_=false;pulses_=0;}
  Action feed(uint32_t now,const int32_t xyz[3]) {
    if(!ready_||(uint32_t)(now-sampleAt_)>300){for(int i=0;i<3;++i)previous_[i]=xyz[i];sampleAt_=now;ready_=true;pulses_=0;return Action::None;}
    if((uint32_t)(now-sampleAt_)<15)return Action::None;
    float delta=0,d[3];for(int i=0;i<3;++i){d[i]=(float)xyz[i]-previous_[i];delta+=d[i]*d[i];previous_[i]=xyz[i];}sampleAt_=now;
    if(delta<800.f*800.f)return Action::None;
    if(pulses_&&(uint32_t)(now-pulseAt_)<120)return Action::None;
    if(!pulses_||(uint32_t)(now-windowAt_)>900){pulses_=0;windowAt_=now;}
    pulseAt_=now;++pulses_;
    if(pulses_==1){
      const float magnitude=sqrtf(delta);
      const float strength=eye_rig::clamp(magnitude/1600.f,.65f,1.3f);
      // Existing PCB screen axes: x=+Y, y=-X. Recoil opposes acceleration.
      // Divide by full 3D magnitude so depth-only bumps stay centered.
      directionX_=-d[1]/magnitude*strength;
      directionY_= d[0]/magnitude*strength;
    }
    return pulses_>=4?Action::Dizzy:pulses_==1?Action::Startle:Action::None;
  }
};
}}
