#pragma once
#include <cmath>
#include <stdint.h>

// Allocation-free home character model. Seconds/radians/pixels, UI core only.
// Pure math so gesture sequences can be tested without hardware.
namespace ui { namespace home_motion {
inline float limit(float v,float a,float b){return v<a?a:v>b?b:v;}
inline float ease(float v){v=limit(v,0,1);return v*v*(3-2*v);}
struct Model {
 float x=0,y=0,vx=0,vy=0,gazeX=0,gazeY=0;
 float roll=0,pitch=0,omega=0,pitchRate=0,agitation=0,loose=0,recovery=0,squeeze=0;
 float now=0,lastMove=0,lastImpact=0,squeezeUntil=-10;
 bool unmoored=false,landed=false;
 bool contact=false;
 void step(float dt,float nextRoll,float nextPitch,float jolt=0){
  if(dt<=0)return;now+=dt;dt=fminf(dt,.1f);contact=false;
  float delta=nextRoll-roll;while(delta>3.14159265f)delta-=6.2831853f;while(delta< -3.14159265f)delta+=6.2831853f;
  float dp=nextPitch-pitch;
  if(fabsf(delta)+fabsf(dp)>.00436f||jolt>.3f){if(now-lastMove>.25f)landed=false;lastMove=now;}
  roll=nextRoll;pitch=nextPitch;
  float old=omega,oldP=pitchRate,blend=1-expf(-dt/.045f);
  omega+=(limit(delta/dt,-18,18)-omega)*blend;
  pitchRate+=(limit(dp/dt,-12,12)-pitchRate)*blend;
  float alpha=limit((omega-old)/dt,-240,240),pa=limit((pitchRate-oldP)/dt,-180,180);
  agitation*=expf(-dt/1.1f);
  agitation=limit(agitation+dt*(fmaxf(0,fabsf(alpha)-35)*.016f+fmaxf(0,fabsf(omega)-3)*.12f+fmaxf(0,fabsf(pa)-30)*.012f+fmaxf(0,jolt-.25f)*2),0,4);
  if(!unmoored&&now-lastMove<.18f&&agitation>1.6f)unmoored=true;
  if(unmoored&&(now-lastMove>.45f||(agitation<.3f&&hypotf(vx,vy)<45)))unmoored=false;
  loose+=((unmoored?1.f:0.f)-loose)*(1-expf(-dt/(unmoored?.12f:.4f)));
  if(now-lastMove<.18f&&(agitation>.7f||unmoored))squeezeUntil=now+.2f;
  squeeze=limit(squeeze+(now<squeezeUntil?dt/.1f:-dt/.18f),0,1);
  bool recovering=loose>.1f&&now-lastMove>.18f;
  recovery+=((recovering?1.f:0.f)-recovery)*(1-expf(-dt/.18f));
  const float gx=sinf(roll)*cosf(pitch),gy=cosf(roll)*cosf(pitch);
  float release=1-powf(cosf(roll/2)*cosf(pitch),2);
  float fx=gx*3200,fy=gy*(release+(1-release)*loose)*3200+sinf(pitch)*1400;
  float stiffness=60.5f*(1-loose),freeDrag=1-.85f*loose;
  float drag=9*(freeDrag+(1.5f-freeDrag)*recovery),coupling=.25f+.75f*loose;
  for(float left=dt;left>0;){float h=fminf(left,1.f/240);left-=h;
   float ix=coupling*limit(alpha*(y-65)+omega*omega*x+2*omega*vy,-5000,5000);
   float iy=coupling*limit(-alpha*x+omega*omega*(y-65)-2*omega*vx+pa*22,-5000,5000);
   vx+=(fx+ix-stiffness*x-drag*vx)*h;vy+=(fy+iy-stiffness*y-drag*vy)*h;
   x+=vx*h;y+=vy*h;
   for(int axis=0;axis<2;++axis){float& p=axis?y:x;float& v=axis?vy:vx;float& tangent=axis?vx:vy;
    const float bound=axis?23.25f:26.5f;
    if(fabsf(p)>bound){bool outward=p*v>0;p=limit(p,-bound,bound);if(outward){float speed=fabsf(v);
     if(!landed&&speed>25){landed=true;contact=true;lastImpact=now;}
     v=speed>55?-v*((.55f+.1f*loose)*(1-recovery)+.25f*recovery):0;
     tangent=copysignf(fmaxf(0,fabsf(tangent)-speed*.65f*(1-loose)),tangent);
     if(speed>55)lastImpact=now;
    }}
   }
  }
  float tx=0,ty=0,quiet=now-fmaxf(lastMove,lastImpact);
  if(squeeze==0&&loose<.1f&&quiet>5.f&&hypotf(vx,vy)<5){
   float t=quiet-5.f,phase=fmodf(t,7.f),amount=phase<.65f?ease(phase/.65f):phase<2.65f?1:1-ease((phase-2.65f)/.8f);
   const float looks[4][2]={{4,-3},{-3,3},{2,4},{-4,-2}};int i=(int)(t/7.f)%4;
   tx=limit(x+looks[i][0]*amount,-26.5f,26.5f)-x;ty=limit(y+looks[i][1]*amount,-23.25f,23.25f)-y;
  }
  float g=1-expf(-dt/.12f);gazeX+=(tx-gazeX)*g;gazeY+=(ty-gazeY)*g;
  gazeX=limit(gazeX,-26.5f-x,26.5f-x);gazeY=limit(gazeY,-23.25f-y,23.25f-y);
 }
};
}}
