#include "ui/kami_motion.h"
#include "hal/hal_imu.h"
#include <Arduino.h>
namespace ui { namespace kami_motion {
using namespace kami_expression;
static Action current=Action::None;
static uint32_t since=0,extra=0,lastPoll=0,heldAge=0;
static bool held=false,diagnostic=false;
static MotionDetector detector;
static float recoilX=0,recoilY=0;
static int32_t cached[3]={};
static uint32_t cachedAt=0,pollPeriod=20,reads=0,failures=0,totalUs=0,maxUs=0;
static bool valid=false;
void resetDetector(){detector.reset();diagnostic=true;}
void reset(){current=Action::None;held=false;extra=0;diagnostic=false;detector.reset();recoilX=recoilY=0;valid=false;}
bool active(uint32_t now){return current!=Action::None&&(held||current==Action::Sleep||now-since<duration(current,extra));}
const char* currentName(uint32_t now){return name(active(now)?current:Action::None);}
Frame frame(uint32_t now){
  if(!active(now))return Frame{};
  uint32_t age=held?heldAge:now-since;
  Frame f=sample(current,age,extra);
  if(current==Action::Startle){float v=softSpring(age);f.recoilX=recoilX*v;f.recoilY=recoilY*v;}
  return f;
}
void trigger(Action a,uint32_t now){
  if(active(now)&&current==Action::Wake&&(a==Action::Startle||a==Action::Dizzy))return;
  if(active(now)&&current==Action::Dizzy&&a==Action::Startle)return;
  if(active(now)&&current==Action::Dizzy&&a==Action::Dizzy){extra=now-since;return;}
  if(active(now)&&a==Action::Blink)return;
  current=a;since=now;held=false;extra=0;
  recoilX=recoilY=0;
  Serial.printf("[kami-motion] action=%s\n",name(a));
}
void hold(Action a,uint32_t age){current=a;held=true;heldAge=age;extra=0;}
void inject(uint32_t now,const int32_t xyz[3]){
  const Action a=detector.feed(now,xyz);
  if(a==Action::None)return;
  trigger(a,now);
  if(a==Action::Startle&&current==Action::Startle){recoilX=detector.directionX();recoilY=detector.directionY();}
}
bool acceleration(uint32_t now,int32_t xyz[3]){
  if(!valid||now-cachedAt>100)return false;
  for(int i=0;i<3;++i)xyz[i]=cached[i];return true;
}
void printStats(uint32_t now){
  Serial.printf("[kami-imu] ms=%lu reads=%lu failures=%lu total_us=%lu max_us=%lu period_ms=%lu age_ms=%lu xyz=%ld,%ld,%ld direction=%.3f,%.3f\n",
    (unsigned long)now,(unsigned long)reads,(unsigned long)failures,(unsigned long)totalUs,(unsigned long)maxUs,
    (unsigned long)pollPeriod,(unsigned long)(valid?now-cachedAt:0xffffffffu),
    (long)cached[0],(long)cached[1],(long)cached[2],recoilX,recoilY);
}
void poll(uint32_t now,bool enabled,bool homePhysics){
  if(diagnostic){valid=false;return;}
  if(!enabled||held||current==Action::Sleep){detector.reset();valid=false;return;}
  if(now-lastPoll<pollPeriod)return;
  // One attempt per service tick, never catch up after stalls. No task/queue/heap.
  lastPoll=now;
  if(!hal::imu::isReady()){valid=false;pollPeriod=1000;return;}
  int32_t accel[3];uint32_t start=micros();
  bool ok=hal::imu::readAcceleration(accel);
  uint32_t elapsed=micros()-start;++reads;totalUs+=elapsed;if(elapsed>maxUs)maxUs=elapsed;
  // A bad/shared bus must not turn into a 50Hz timeout loop. No inline retries.
  pollPeriod=ok&&elapsed<10000?20:1000;
  if(!ok){++failures;valid=false;detector.reset();return;}
  for(int i=0;i<3;++i)cached[i]=accel[i];cachedAt=now;valid=true;
  if(current==Action::Wake&&active(now))detector.reset();
  else if(!homePhysics)inject(now,accel);
}
}}
