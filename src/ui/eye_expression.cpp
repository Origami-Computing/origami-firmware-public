#include "ui/eye_rig.h"
#include "ui/timer_controls.h"
#include "hal/hal_display.h"
#include <algorithm>

namespace ui { namespace eye_rig {
namespace {
struct Point {float x,y;};
struct Stroke {Point a,b;float nx,ny,r,lo,hi;};
// UI-core-only scratch, no heap allocations and no large UI stack frames.
static Stroke strokes[160];
static Span spans[160];
static int32_t coverage[280];
static uint16_t pixels[280];
static unsigned count;
void line(Point a,Point b,float weight){
  if(count>=160)return;
  float dx=b.x-a.x,dy=b.y-a.y,len=sqrtf(dx*dx+dy*dy),r=weight*.5f;
  strokes[count++]={a,b,len>0?-dy*r/len:0,len>0?dx*r/len:0,r,fminf(a.y,b.y)-r,fmaxf(a.y,b.y)+r};
}
bool section(const Stroke& s,float y,Span& span){
  if(y<s.lo||y>s.hi)return false;
  float lo=1e9f,hi=-1e9f;
  auto add=[&](float x){lo=fminf(lo,x);hi=fmaxf(hi,x);};
  for(Point p:{s.a,s.b}){float dy=y-p.y;if(fabsf(dy)<=s.r){float d=sqrtf(fmaxf(0,s.r*s.r-dy*dy));add(p.x-d);add(p.x+d);}}
  Point q[4]={{s.a.x+s.nx,s.a.y+s.ny},{s.b.x+s.nx,s.b.y+s.ny},{s.b.x-s.nx,s.b.y-s.ny},{s.a.x-s.nx,s.a.y-s.ny}};
  for(int i=0;i<4;++i){Point a=q[i],b=q[(i+1)%4];if(y>=fminf(a.y,b.y)&&y<fmaxf(a.y,b.y))add(a.x+(b.x-a.x)*(y-a.y)/(b.y-a.y));}
  span={lo,hi};return hi>lo;
}
void raster(TFT_eSPI& target,int x,int y,int w,int h,uint16_t bg,bool composite,int samples=4,uint16_t ink=0x18c3){
  if(w<=0||w>280||h<=0)return;
  const bool swap=target.getSwapBytes();target.setSwapBytes(false);
  for(int row=0;row<h;++row){
    memset(coverage,0,w*sizeof(int32_t));
    for(int sub=0;sub<samples;++sub){
      unsigned n=0;float scan=y+row+(sub+.5f)/samples;
      for(unsigned i=0;i<count;++i)if(section(strokes[i],scan,spans[n]))++n;
      std::sort(spans,spans+n,[](const Span& a,const Span& b){return a.a<b.a;});
      for(unsigned i=0;i<n;){
        float l=spans[i].a,r=spans[i].b;++i;
        while(i<n&&spans[i].a<=r){r=fmaxf(r,spans[i].b);++i;}
        l=clamp(l-x,0,w);r=clamp(r-x,0,w);if(r<=l)continue;
        const int first=(int)l,last=(int)r;
        const float unit=65536.f/samples;
        if(first==last) {
          if(first<w)coverage[first]+=(int)((r-l)*unit+.5f);
        } else {
          coverage[first]+=(int)((first+1.f-l)*unit+.5f);
          // Only the two edge pixels need floating-point coverage. The span's
          // interior is opaque at this sample, so use integer additions.
          for(int col=first+1;col<last;++col)coverage[col]+=65536/samples;
          if(last<w)coverage[last]+=(int)((r-last)*unit+.5f);
        }
      }
    }
    int first=0,last=w;
    if(composite){while(first<last&&!coverage[first])++first;while(last>first&&!coverage[last-1])--last;}
    for(int col=first;col<last;++col){
      int a=(int)clamp((coverage[col]+128)/257,0,255);
      uint16_t b=composite?target.readPixel(x+col,y+row):bg;
      if(a==0 || a==255){uint16_t c=a?ink:b;pixels[col]=(c>>8)|(c<<8);continue;}

      int red=(((ink>>11)&31)*a+((b>>11)&31)*(255-a)+127)/255;
      int green=(((ink>>5)&63)*a+((b>>5)&63)*(255-a)+127)/255;
      int blue=((ink&31)*a+(b&31)*(255-a)+127)/255;
      uint16_t c=(red<<11)|(green<<5)|blue;pixels[col]=(c>>8)|(c<<8);
    }
    if(last>first)target.pushImage(x+first,y+row,last-first,1,pixels+first);
  }
  target.setSwapBytes(swap);
}
void eye(kami_expression::Shape shape,float cx,float cy,float nominal,float squash,float angle,bool right){
  using kami_expression::Shape;
  const float unit=nominal/67.f,weight=11*(shape==Shape::Spiral?.42f:1)*unit;
  Point previous{};bool first=true;
  auto point=[&](float x,float y){
    if(right&&shape!=Shape::Spiral)x=67-x;
    if(shape==Shape::Spiral)x+=7;
    x-=33.5f;y-=45.5f;
    const float c=cosf(angle),s=sinf(angle);
    Point p={cx+(x*c-y*s)*unit,cy+(x*s+y*c)*unit*squash};
    if(!first)line(previous,p,weight*fminf(1.f,squash));previous=p;first=false;
  };
  if(shape==Shape::Spiral){for(int i=0;i<=64;++i){float a=i/64.f*4*3.14159265f,r=23*i/64.f;point(33.5f+cosf(a)*r,45.5f+sinf(a)*r);}return;}
  if(shape==Shape::Squeeze){
    for(int leg=0;leg<2;++leg)for(int i=0;i<=12;++i){float t=i/12.f,u=1-t;
      point(u*u*(leg?56:10)+2*u*t*34+t*t*(leg?10:56),u*u*(leg?48:34)+2*u*t*(leg?57:39)+t*t*(leg?62:48));}
    return;
  }
  for(int i=0;i<=32;++i){float t=i/32.f,u=1-t;
    if(shape==Shape::Bowl)point(u*u*u*6+3*u*u*t*6+3*u*t*t*61+t*t*t*61,u*u*u*40+3*u*u*t*68+3*u*t*t*68+t*t*t*40);
    else point(u*u*u*7+3*u*u*t*7+3*u*t*t*60+t*t*t*60,u*u*u*60+3*u*u*t*24.7f+3*u*t*t*24.7f+t*t*t*60);
  }
}
}
void drawExpression(TFT_eSPI& t,int x,int y,int w,int h,const Pose& pose,const kami_expression::Frame& frame,uint16_t bg,float nominal,float spacing,float ox,float oy){
  using kami_expression::Shape;
  if(frame.shape==Shape::Open){
    Pose p=frame.neutral?kPresets[0].pose:pose;
    if(frame.startleMix>=0){
      Pose startled=preset("startled")->pose;
      // Feedback on COM14: 25% less peak height, while retaining D's open lids.
      startled.scale=1.125f; // original startled scale 1.5 * 0.75
      p=interpolate(kPresets[0].pose,startled,frame.startleMix);
      if(nominal<=0)nominal=fminf(w/2.2f,h*67.f/91.f);
      // The web preview leaves a narrow gap at full surprise. Home's tighter
      // neutral spacing would merge the enlarged eyes; open that gap with D.
      float surpriseSpacing=(1+(startled.scale-1)*.4f+.03f)/1.2f;
      spacing=mix(spacing,fmaxf(spacing,surpriseSpacing),frame.startleMix);
      // Clamp to the available canvas; never crop enlarged eyes or the status bar.
      float ew=nominal*(1+(p.scale-1)*.4f),eh=nominal*91.f/67.f*p.scale*frame.squash;
      float distance=centerSeparation(nominal,spacing);
      float fit=fminf(1.f,fminf(w/(distance+ew),h/fmaxf(1,eh)));
      float roomX=fmaxf(0,(w-(distance+ew)*fit)*.5f-1);
      float roomY=fmaxf(0,(h-eh*fit)*.5f-1);
      ox=clamp(ox+frame.recoilX*nominal,-roomX,roomX);
      oy=clamp(oy+frame.recoilY*nominal,-roomY,roomY);
    }
    draw(t,x,y,w,h,p,frame.squash*frame.pop,bg,nominal,ox,oy,false,255,0,-1,spacing);return;
  }
  if(nominal<=0)nominal=fminf(w/2.2f,h*67.f/91.f);
  nominal*=fminf(1.f,w/(centerSeparation(nominal,spacing)+nominal));
  float separation=centerSeparation(nominal,spacing),cx=x+w*.5f+ox,cy=y+h*.5f+oy;
  count=0;
  for(int i=0;i<2;++i)eye(frame.shape,cx+(i?1:-1)*separation*.5f,cy,nominal,frame.squash,frame.spinMs/(1300.f*(i?1.23f:1))*2*3.14159265f,i);
  raster(t,x,y,w,h,bg,false,nominal<16?8:4);
}
// Opaque lid-to-arc morph, matching the web's rounded U geometry.
void drawWakeFace(TFT_eSPI& t,int x,int y,int w,int h,float amount){
  auto smooth=[](float v){v=clamp(v,0,1);return v*v*(3-2*v);};
  if(amount<=.5f){
    draw(t,x,y,w,h,kPresets[0].pose,1,0xffff,88,0,0,false,255,0,-1,kHomeSpacing,1-smooth(amount*2));return;
  }
  const float bend=smooth((amount-.5f)*2),unit=88.f/67;
  const float angle=-10*bend*3.14159265f/180,c=cosf(angle),s=sinf(angle);
  count=0;
  for(int side=0;side<2;++side){
    Point previous{};
    for(int i=0;i<=24;++i){float t=i/24.f,u=1-t;
      float px=u*u*u*(6.5f+.5f*bend)+3*u*u*t*(24.5f-17.5f*bend)+3*u*t*t*(42.5f+17.5f*bend)+t*t*t*(60.5f-.5f*bend);
      float py=(u*u*u+t*t*t)*(45.5f+14.5f*bend)+(3*u*u*t+3*u*t*t)*(45.5f-20.8f*bend);
      px=(px-33.5f)*unit+(side?1:-1)*centerSeparation(88,kHomeSpacing)*.5f;
      py=(py-45.5f)*unit;
      Point p={x+w*.5f+px*c-py*s,y+h*.5f+px*s+py*c};
      if(i)line(previous,p,11*unit);previous=p;
    }
  }
  raster(t,x,y,w,h,0xffff,false,4);
}
void drawRingingEyes(TFT_eSPI& t,uint32_t ms,uint16_t bg) {
  const float unit=92.f/67.f,sep=92.f*1.08f,rad=3.14159265f/180.f;
  const float pair=timer_controls::ringingPose(ms,0).pair*rad;
  count=0;
  for(int i=0;i<2;++i) {
    const float local=timer_controls::ringingPose(ms,i).local*rad,cx=(i?1:-1)*sep*.5f,outer=(i?21.5f:-21.5f)*unit;
    auto transform=[&](float x,float y){float xx=x*cosf(local)-y*sinf(local)+cx,yy=x*sinf(local)+y*cosf(local);return Point{140+xx*cosf(pair)-yy*sinf(pair),140+xx*sinf(pair)+yy*cosf(pair)};};
    line(transform(outer,-23.5f*unit),transform(-outer,0),11*unit);
    line(transform(-outer,0),transform(outer,23.5f*unit),11*unit);
  }
  // The rotated stroke bounds stay within y=93..187. Two vertical samples
  // retain fractional horizontal coverage without oversampling empty space.
  raster(t,24,90,232,100,bg,false,2,0);
}
void drawHomeMorph(TFT_eSPI& t,int x,int y,int w,int h,float squeeze,float blink,float nominal,float ox,float oy,int firstRow,int lastRow){
  if(lastRow<0)lastRow=h;
  auto smooth=[](float v){v=clamp(v,0,1);return v*v*(3-2*v);};
  if(squeeze<.6f){
    Pose p=kPresets[0].pose;
    float closure=squeeze>0?0:(1-clamp(blink,0,1))*.5f;
    p.left.topY=p.left.bottomY=p.right.topY=p.right.bottomY=closure;
    float aperture=squeeze>0?1-smooth(squeeze/.6f):1;
    draw(t,x,y,w,h,p,1,0xffff,nominal,ox,oy,false,255,firstRow,lastRow,kHomeSpacing,aperture);
    return;
  }
  const float bend=smooth((squeeze-.6f)/.4f),unit=nominal/67.f;
  const float separation=centerSeparation(nominal,kHomeSpacing);
  count=0;
  for(int i=0;i<2;++i){
    float cx=x+w*.5f+ox+(i?1:-1)*separation*.5f,cy=y+h*.5f+oy;
    float outer=(i?21.5f:-21.5f)*unit,inner=-outer;
    line({cx+outer,cy-23.5f*bend*unit},{cx+inner,cy},11*unit);
    line({cx+inner,cy},{cx+outer,cy+23.5f*bend*unit},11*unit);
  }
  raster(t,x,y+firstRow,w,lastRow-firstRow,0xffff,false,4);
}
void drawExpressionMarks(TFT_eSPI& t,int x,int y,int w,int h,float cx,float cy,float nominal,const kami_expression::Frame& f,uint16_t bg){
  if(f.burstMs<0&&f.zzzMs<0)return;
  count=0;float unit=nominal/88.f;
  if(f.burstMs>=0){
    for(int i=0;i<12;++i){
      float delay=30*(1+sinf(i*4.1f)),p=clamp((f.burstMs-delay)/580.f,0,1);
      if(p<=0||p>=1)continue;
      float a=(i+.5f+sinf(i*8.1f)*.275f)/12*2*3.14159265f;
      float rx=cosf(a)*105*(1+sinf(i*3.8f)*.055f),ry=sinf(a)*78*(1+cosf(i*4.7f)*.055f);
      float angle=atan2f(ry,rx),dx=cosf(angle),dy=sinf(angle),move=-8+16*(1-powf(1-p,3));
      float len=9+7*(1+sinf(i*2.9f))*.5f;
      float alpha=p<.1f?p/.1f:p<.48f?1:(1-p)/.52f;
      // Fade by reducing coverage width; preserves antialiasing on RGB565.
      Point a0={clamp(cx+(rx+dx*move)*unit,x+2,x+w-2),clamp(cy+(ry+dy*move)*unit,y+4,y+h-6)};
      Point b0={clamp(cx+(rx+dx*(move+len))*unit,x+2,x+w-2),clamp(cy+(ry+dy*(move+len))*unit,y+4,y+h-6)};
      line(a0,b0,(2.1f+.65f*cosf(i*2.3f))*unit*alpha);
    }
  }
  if(f.zzzMs>=0){
    for(int i=0;i<3;++i){
      float p=f.zzzMs>=3400?i/3.f:fmodf(f.zzzMs/3400.f+i/3.f,1.f);
      float size=(12+10*p)*unit,xx=cx+(76+34*p)*unit,yy=cy+(-29-39*p)*unit;
      // Three stroke Zs stay legible without a font/bitmap dependency.
      float ww=size*.6f,hh=size*.7f;
      line({xx,yy-hh},{xx+ww,yy-hh},unit*1.5f);
      line({xx+ww,yy-hh},{xx,yy},unit*1.5f);
      line({xx,yy},{xx+ww,yy},unit*1.5f);
    }
  }
  raster(t,x,y,w,h,bg,true,nominal<16?8:4);
}
}}
