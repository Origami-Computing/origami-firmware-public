#pragma once
#include <cmath>
#include <cstring>
#include "assets/kami_eye_rig.h"

// No Arduino dependency: the same rasterizer is exercised by the host preview/tests.
namespace ui { namespace eye_rig {
inline float clamp(float x,float lo,float hi) { return x<lo?lo:x>hi?hi:x; }
inline float mix(float a,float b,float t) { return a+(b-a)*t; }
inline const Preset* preset(const char* name) {
  for (const auto& p:kPresets) if (name && !strcmp(name,p.name)) return &p;
  return nullptr;
}
inline float bezier(float t,float x1,float y1,float x2,float y2) {
  if(t<=0) return 0; if(t>=1) return 1;
  float lo=0,hi=1,u=t;
  for(int i=0;i<14;++i) {
    u=(lo+hi)*.5f;
    float x=3*(1-u)*(1-u)*u*x1+3*(1-u)*u*u*x2+u*u*u;
    if(x<t)lo=u;else hi=u;
  }
  return 3*(1-u)*(1-u)*u*y1+3*(1-u)*u*u*y2+u*u*u;
}
inline float blink(uint32_t age,uint32_t hold=0) {
  if(age<60) return mix(1,.05f,bezier(age/60.f,.42f,0,.58f,1));
  if(age<60+hold) return .05f;
  if(age<200+hold) return mix(.05f,1,bezier((age-60-hold)/140.f,.34f,1.56f,.64f,1));
  return 1;
}
inline Lid interpolate(const Lid& a,const Lid& b,float t) {
  // Interpolate actual SVG circle centers/radii. These are linear in these fields
  // except size, which is itself interpolated, so this matches the web masks.
  return {mix(a.topY,b.topY,t),mix(a.topX,b.topX,t),mix(a.bottomY,b.bottomY,t),
          mix(a.bottomX,b.bottomX,t),mix(a.size,b.size,t)};
}
inline Pose interpolate(const Pose& a,const Pose& b,float t) {
  return {interpolate(a.left,b.left,t),interpolate(a.right,b.right,t),mix(a.scale,b.scale,t)};
}
// A quiet thinking pose from the existing design-language preset. Keep the
// canonical scale; only the lids settle, with no roaming or curious flashes.
inline Pose quietThinkingPose(float amount=1) {
  const auto* thinking=preset("thinking");
  return interpolate(kPresets[0].pose,thinking?thinking->pose:kPresets[0].pose,.3f*clamp(amount,0,1));
}
struct Span { float a,b; };
inline int subtract(Span* spans,int n,float a,float b) {
  Span result[4]; int count=0;
  for(int i=0;i<n;++i) {
    const Span s=spans[i];
    if(b<=s.a || a>=s.b) result[count++]=s;
    else { if(a>s.a)result[count++]={s.a,a}; if(b<s.b)result[count++]={b,s.b}; }
  }
  for(int i=0;i<count;++i)spans[i]=result[i];
  return count;
}

// Four vertical coverage samples (eight for small/closed eyes), exact horizontal
// interval coverage. No scaled
// bitmap, thresholded alpha, or per-frame allocation. The output contains
// horizontal coverage DELTAS; integrate once after both eyes have contributed.
// This avoids visiting every solid interior pixel eight times per output row.
// Circles are subtracted in
// eye coordinates BEFORE the complete shape is transformed, including blink.
inline void eyeRow(int32_t* coverage,int count,int xOrigin,int row,
                   float centerX,float centerY,float width,float height,const Lid& lid,float aperture=1) {
  if(width<=0 || height<=0)return;
  const int samples=height<32?8:4;
  const float sx=width/67.f, sy=height/91.f;
  const float left=centerX-width*.5f,top=centerY-height*.5f;
  for(int sub=0;sub<samples;++sub) {
    float y=(row+(sub+.5f)/samples-top)/sy;
    if(y<0 || y>=91)continue;
    float v=y*8;int index=(int)v;float f=v-index;
    auto a=kOutline[index];auto b=kOutline[index+1];
    if(a[0]>a[1])continue;
    // At the last contour sample, taper towards its midpoint, not a sentinel.
    float nextL=b[0]>b[1]?(a[0]+a[1])*.5f:b[0];
    float nextR=b[0]>b[1]?nextL:b[1];
    Span spans[4]={{mix(a[0],nextL,f)/512.f,mix(a[1],nextR,f)/512.f}};int n=1;
    float radius=clamp(lid.size,1,800);
    const float cx[2]={33.5f+lid.topX*67,33.5f+lid.bottomX*67};
    const float cy[2]={lid.topY*91-radius,91*(1-lid.bottomY)+radius};
    for(int c=0;c<2 && n;++c) {
      float dy=y-cy[c];
      if(fabsf(dy)<radius) { float half=sqrtf(fmaxf(0,radius*radius-dy*dy)); n=subtract(spans,n,cx[c]-half,cx[c]+half); }
    }
    if(aperture<1){
      const float ah=11+80*aperture,aw=54+13*aperture;
      float dy=fabsf(y-45.5f),half=aw*.5f;
      if(dy>ah*.5f)continue;
      if(dy>ah*.5f-5.5f){float d=dy-(ah*.5f-5.5f);half-=5.5f-sqrtf(fmaxf(0,30.25f-d*d));}
      for(int i=0;i<n;++i){spans[i].a=fmaxf(spans[i].a,33.5f-half);spans[i].b=fminf(spans[i].b,33.5f+half);}
    }
    for(int i=0;i<n;++i) {
      float l=clamp(left+spans[i].a*sx-xOrigin,0,(float)count);
      float r=clamp(left+spans[i].b*sx-xOrigin,0,(float)count);
      if(r<=l)continue;
      int first=(int)l,last=(int)r;
      const int unit=height<32?8192:16384;
      if(first==last){
        int amount=(int)((r-l)*unit+.5f);
        coverage[first]+=amount;
        if(first+1<count)coverage[first+1]-=amount;
      }else{
        int lf=(int)((l-first)*unit+.5f),rf=(int)((r-last)*unit+.5f);
        coverage[first]+=unit-lf;
        if(first+1<count)coverage[first+1]+=lf;
        if(last<count)coverage[last]-=unit-rf;
        if(last+1<count)coverage[last+1]-=rf;
      }
    }
  }
}
}} // namespace ui::eye_rig
