#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace games { namespace trex {
constexpr float kGravity=1500, kJump=-430, kBaseSpeed=150, kMaxSpeed=320, kPlayerX=24;
enum class Kind : uint8_t {Small, Tall, Pair, Bird};
struct Box {float x,y,w,h;};
inline bool overlaps(Box a,Box b){return a.x<b.x+b.w&&a.x+a.w>b.x&&a.y<b.y+b.h&&a.y+a.h>b.y;}
inline float width(Kind k){return k==Kind::Small?17:k==Kind::Tall?25:k==Kind::Pair?34:46;}
struct Obstacle {
 bool active;float x;Kind kind;
 Obstacle(bool enabled=false,float position=0,Kind type=Kind::Small):active(enabled),x(position),kind(type){}
};
struct Runner {
 int width=280,height=240;
 bool ready=true,paused=false,dead=false,grounded=true,duck=false,released=false;
 float y=0,vy=0,speed=kBaseSpeed,distance=0,nextSpawn=80;
 uint32_t simMs=0,remainder=0,score=0,best=0,jumps=0,landings=0,rng=1;
 Obstacle obstacles[5];
 Kind lastKind=Kind::Small;
 unsigned repeats=0;
 // Continuous active-play progression: half the speed increase at 90 seconds,
 // still increasing after many minutes, bounded for this 280px viewport.
 float difficulty()const{return float(simMs)/(float(simMs)+90000.f);}
 float targetSpeed()const{return kBaseSpeed+(kMaxSpeed-kBaseSpeed)*difficulty();}
 float ground() const{return height-42.f;}
 void reset(bool wait=true){const auto hi=best,seed=rng;const int w=width,h=height;*this=Runner();best=hi;rng=seed;width=w;height=h;ready=wait;}
 uint32_t random(){rng=rng*1664525u+1013904223u;return rng;}
 void dial(){if(ready||dead){reset(false);return;}paused=!paused;}
 void pressJump(){if(ready||dead)reset(false);if(paused||!grounded)return;duck=false;released=false;grounded=false;vy=kJump;++jumps;}
 void releaseJump(){released=true;}
 void pressDuck(){if(ready||dead||paused)return;duck=true;if(!grounded)vy=std::max(vy,360.f);}
 void releaseDuck(){duck=false;}
 int playerBoxes(Box* b) const {
   const float x=kPlayerX,top=ground()-47+y;
   if(duck&&grounded){b[0]={x+1,top+18,55,25};return 1;}
   b[0]={x+22,top,17,16};b[1]={x+1,top+18,30,9};
   b[2]={x+10,top+35,14,8};b[3]={x+1,top+24,29,5};
   b[4]={x+5,top+30,21,4};b[5]={x+9,top+34,15,4};return 6;
 }
 int obstacleBoxes(const Obstacle&o,Box*b)const{
   const float x=o.x,g=ground();
   if(o.kind==Kind::Bird){const float t=g-65;
     b[0]={x+15,t+15,16,5};b[1]={x+18,t+21,24,6};b[2]={x+2,t+14,4,3};
     b[3]={x+6,t+10,4,7};b[4]={x+10,t+8,6,9};return 5;}
   if(o.kind==Kind::Tall){b[0]={x,g-38,7,38};b[1]={x+8,g-50,7,49};b[2]={x+13,g-40,10,38};return 3;}
   const int n=o.kind==Kind::Pair?2:1;
   for(int i=0;i<n;++i){const float xx=x+17*i;b[i*3]={xx,g-28,5,27};b[i*3+1]={xx+4,g-35,6,34};b[i*3+2]={xx+10,g-31,7,14};}return n*3;
 }
 bool collides(const Obstacle&o)const{Box a[6],b[6];int na=playerBoxes(a),nb=obstacleBoxes(o,b);for(int i=0;i<na;++i)for(int j=0;j<nb;++j)if(overlaps(a[i],b[j]))return true;return false;}
 float spacing(Kind previous)const{
   // Full standing/ducking footprint, full jump, and >=160ms recovery.
   // Anticipate acceleration while both obstacles travel to the player.
   const float recovery=.16f+.18f*(1-difficulty());
   return games::trex::width(previous)+55+std::min(kMaxSpeed,speed+8)*(.58f+recovery);
 }
 Kind chooseKind(){
   // Use upper LCG bits: the low bits repeat in short, predictable cycles.
   const unsigned roll=(random()>>16)%100;
   const float level=difficulty();
   Kind kind=Kind::Small;
   if(simMs>=45000&&roll<10+unsigned(20*level))kind=Kind::Bird;
   else if(simMs>=25000&&roll<35+unsigned(15*level))kind=Kind::Pair;
   else if(simMs>=12000&&roll<70)kind=Kind::Tall;
   // After the introduction, break long same-obstacle runs and encourage
   // jump/duck transitions without ever reducing the physical safety gap.
   if(simMs>=45000&&kind==lastKind&&repeats>=2)
     kind=kind==Kind::Bird?Kind::Small:Kind::Bird;
   repeats=kind==lastKind?repeats+1:1;lastKind=kind;
   return kind;
 }
 void step(){
   constexpr float dt=.008f;simMs+=8;
   const float dx=speed*dt;distance+=dx;score=uint32_t(distance/6);
   speed=targetSpeed();
   if(!grounded){
     // Exact constant-acceleration integration inside small collision substeps.
     if(released&&y<=-36&&vy<-180)vy=-180;
     y+=vy*dt+.5f*kGravity*dt*dt;vy+=kGravity*dt;
     if(y>=0){y=vy=0;grounded=true;++landings;}
   }
   for(auto&o:obstacles){if(!o.active)continue;o.x-=dx;if(o.x+games::trex::width(o.kind)<0){o.active=false;continue;}if(collides(o)){dead=true;best=std::max(best,score);return;}}
   if(distance>=nextSpawn){for(auto&o:obstacles)if(!o.active){
     o={true,float(width)+12,chooseKind()};
     // Irregular breathing room shrinks gradually, never below spacing().
     nextSpawn=distance+spacing(o.kind)+float((random()>>16)%101)*(1-.65f*difficulty());break;
   }}
 }
 void advance(uint32_t ms){
   if(ready||paused||dead)return;
   // Bound pathological stalls; all clocks slow together, never just motion.
   remainder+=std::min(ms,250u);
   while(remainder>=8&&!dead){remainder-=8;step();}
 }
};
} }
