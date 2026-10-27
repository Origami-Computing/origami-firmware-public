#include "ui/home_eyes.h"
#include "ui/greeting_hand.h"
#include "ui/outro_greeting.h"
#include "core/pixel_rle.h"
#include "ui/eye_rig.h"
#include "ui/kami_motion.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <SD.h>
#include <esp_random.h>
#include <esp_heap_caps.h>
#include "config/ui_layout.h"
#include "hal/hal_display.h"
#include "hal/hal_imu.h"
#include "core/watchdog.h"

namespace ui { namespace home_eyes {
namespace rig=eye_rig;
const uint8_t kFrameSequence[kSequenceLen]={0,1,2,1};
const uint16_t kFrameDurationsMs[HOME_SCREEN_NUM_FRAMES]={60,60,140};
static bool loaded=false,frozen=false,complete=false,tapEnabled=false;
static bool blinking=false,transitioning=false;
static uint32_t blinkStart=0,nextBlink=0,hold=0,freezeAt=0,transitionStart=0,transitionMs=0;
static float offsetX=0,offsetY=0,homeSqueeze=0;

static rig::Pose current=rig::kPresets[0].pose,from=current,target=current;
static const char* emotion="neutral";
static KamiAliveTune alive=defaultKamiAliveTune();
static uint32_t frames=0,lastRenderUs=0,maxRenderUs=0;
static uint32_t heartStart=0;
static bool hearts=false;
static constexpr int kWidth=252,kHeight=170,kTop=42;
static int previousTop=-1,previousBottom=-1;
static bool previousExpression=false;
static kami_expression::Frame blinkFrame;

// Lossless caches retain the original sprite words, with one shared decode
// surface. All preparation/painting is UI-task owned; painters borrow the surface
// only until the next frame lookup. No allocation occurs during animation.
static constexpr int kCacheW=184,kCacheH=132,kCacheFrames=78;
static constexpr int kWakeW=196,kWakeH=144,kWakeFrames=41;
static uint16_t* motionCache=nullptr;
static uint16_t* wakeCache=nullptr;
static uint16_t* decodedFrame=nullptr;
static size_t motionOffsets[kCacheFrames+1],wakeOffsets[kWakeFrames+1];
static int wakeInkTop[kWakeFrames],wakeInkBottom[kWakeFrames];
static bool cacheAttempted=false,wakeCacheAttempted=false;
static const uint16_t* decodedOwner=nullptr;
static int decodedIndex=-1;

template<class Render>
static uint16_t* prepareCache(TFT_eSPI& t,int w,int h,int count,size_t* offsets,Render render){
  if(!decodedFrame){
    decodedFrame=(uint16_t*)heap_caps_malloc(kWakeW*kWakeH*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!decodedFrame)return nullptr;
  }
  TFT_eSprite sprite(&t);sprite.setColorDepth(16);
  if(!sprite.createSprite(w,h))return nullptr;
  // Count first, then allocate exactly once. Never allocate the old full cache
  // or a growing buffer, which would recreate its peak/fragmentation pressure.
  offsets[0]=0;
  for(int i=0;i<count;++i){
    render(sprite,i);
    offsets[i+1]=offsets[i]+core::pixel_rle::encode((uint16_t*)sprite.getPointer(),w*h);
    core::watchdog::feed();
  }
  uint16_t* cache=(uint16_t*)heap_caps_malloc(offsets[count]*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
  if(cache){
    for(int i=0;i<count;++i){
      render(sprite,i);
      // Both passes use identical deterministic geometry. Verify the size
      // before writing so a future renderer change cannot overrun the cache.
      auto* pixels=(uint16_t*)sprite.getPointer();
      if(core::pixel_rle::encode(pixels,w*h)!=offsets[i+1]-offsets[i]){
        free(cache);cache=nullptr;break;
      }
      core::pixel_rle::encode(pixels,w*h,cache+offsets[i]);
      core::watchdog::feed();
    }
  }
  sprite.deleteSprite();
  if(cache)Serial.printf("[eye-cache] lossless bytes=%u frames=%u scratch=%u\n",unsigned(offsets[count]*2),count,kWakeW*kWakeH*2);
  return cache;
}
static const uint16_t* framePixels(const uint16_t* cache,const size_t* offsets,int index,int count,size_t pixels){
  if(!cache || !decodedFrame || index<0 || index>=count)return nullptr;
  if(decodedOwner!=cache || decodedIndex!=index){
    if(!core::pixel_rle::decode(cache+offsets[index],offsets[index+1]-offsets[index],decodedFrame,pixels))return nullptr;
    decodedOwner=cache;decodedIndex=index;
  }
  return decodedFrame;
}
static void prepareMotionCache(TFT_eSPI& t){
  if(cacheAttempted)return;cacheAttempted=true;
  motionCache=prepareCache(t,kCacheW,kCacheH,kCacheFrames,motionOffsets,[](TFT_eSprite& sprite,int i){
    if(i<=20)rig::drawHomeMorph(sprite,0,0,kCacheW,kCacheH,i/20.f,1,88,0,0);
    else if(i>=47){
      const float a=(i-47)/30.f,nominal=88+(16.f/2.2f-88)*a;
      rig::draw(sprite,0,0,kCacheW,kCacheH,rig::kPresets[0].pose,1,TFT_WHITE,nominal,
                0,0,false,255,0,-1,rig::kHomeSpacing+(1-rig::kHomeSpacing)*a);
    }else rig::drawExpression(sprite,0,0,kCacheW,kCacheH,rig::kPresets[0].pose,
                            kami_expression::blink((i-21)*10),TFT_WHITE,88,rig::kHomeSpacing);
  });
}
static void prepareWakeCache(TFT_eSPI& t){
  if(wakeCacheAttempted)return;wakeCacheAttempted=true;
  wakeCache=prepareCache(t,kWakeW,kWakeH,kWakeFrames,wakeOffsets,[](TFT_eSprite& sprite,int i){
    rig::drawWakeFace(sprite,0,0,kWakeW,kWakeH,i/40.f);
    auto* pixels=(uint16_t*)sprite.getPointer();
    int first=kWakeH,last=0;
    for(int row=0;row<kWakeH;++row)for(int col=0;col<kWakeW;++col)if(pixels[row*kWakeW+col]!=0xffff){first=min(first,row);last=max(last,row+1);}
    wakeInkTop[i]=first;wakeInkBottom[i]=last;
  });
  if(!motionCache&&!wakeCache){free(decodedFrame);decodedFrame=nullptr;}
}
static void paintWake(TFT_eSPI& t,uint32_t age,int yOff){
  auto f=wake_greeting::sample(age);
  const int cx=t.width()/2,cy=kTop+kHeight/2+yOff;
  const int index=(int)lroundf(rig::clamp(f.face,0,1)*40);
  const uint16_t* wakePixels=framePixels(wakeCache,wakeOffsets,index,kWakeFrames,kWakeW*kWakeH);
  int top=cy-kWakeH/2+(wakePixels?wakeInkTop[index]:0);
  int bottom=cy-kWakeH/2+(wakePixels?wakeInkBottom[index]:kWakeH);
  if(f.handAlpha>0){top=min(top,(int)floorf(cy+f.handY-16));bottom=max(bottom,(int)ceilf(cy+f.handY+18));}
  top=max(kSystemStatusBarH,top);bottom=min((int)t.height(),bottom);
  int first=previousTop<0?kSystemStatusBarH:min(previousTop,top);
  int last=previousBottom<0?t.height():max(previousBottom,bottom);
  t.fillRect(0,first,t.width(),last-first,TFT_WHITE);
  if(wakePixels){
    bool swap=t.getSwapBytes();t.setSwapBytes(false);
    t.pushImage(cx+(int)lroundf(f.x)-kWakeW/2,cy-kWakeH/2+wakeInkTop[index],kWakeW,wakeInkBottom[index]-wakeInkTop[index],wakePixels+wakeInkTop[index]*kWakeW);
    t.setSwapBytes(swap);
  }else rig::drawWakeFace(t,cx+(int)lroundf(f.x)-kWakeW/2,cy-kWakeH/2,kWakeW,kWakeH,f.face);
  // Coverage-render the white hand with the unchanged wake geometry.
  ui::greeting_hand::draw(t,f,cx,cy);
  blinking=false;nextBlink=millis()+2800;
  previousExpression=true;previousTop=top;previousBottom=bottom;
  hal::display::markDirty(first,last-first);
}

static void sample(uint32_t now) {
  if(!transitioning)return;
  float t=rig::clamp((uint32_t)(now-transitionStart)/(float)transitionMs,0,1);
  current=rig::interpolate(from,target,rig::bezier(t,.37f,0,.63f,1));
  if(t>=1){current=target;transitioning=false;}
}
void drawOutroWave(TFT_eSPI& t,uint32_t age){
 const auto f=outro_greeting::sample(age);
 const int cx=t.width()/2,cy=kTop+kHeight/2;
 t.fillRect(0,0,t.width(),t.height(),TFT_WHITE);
 const int index=int(lroundf(rig::clamp(f.face,0,1)*40));
 const auto* pixels=framePixels(wakeCache,wakeOffsets,index,kWakeFrames,kWakeW*kWakeH);
 if(pixels){const bool swap=t.getSwapBytes();t.setSwapBytes(false);t.pushImage(cx+int(lroundf(f.x))-kWakeW/2,cy-kWakeH/2,kWakeW,kWakeH,pixels);t.setSwapBytes(swap);}
 else rig::drawWakeFace(t,cx+int(lroundf(f.x))-kWakeW/2,cy-kWakeH/2,kWakeW,kWakeH,index/40.f);
 greeting_hand::draw(t,f,cx,cy);
}
static void transition(const rig::Pose& pose,float speed) {
  sample(millis());from=current;target=pose;transitionStart=millis();
  transitionMs=(uint32_t)(350.f/rig::clamp(speed,.1f,3.f));transitioning=true;
}
bool setEmotion(const char* name,bool immediate) {
  const auto* p=rig::preset(name);if(!p)return false;
  emotion=p->name;
  if(immediate){current=target=from=p->pose;transitioning=false;}
  else transition(p->pose,p->speed);
  return true;
}
static uint32_t gap(){return 3500u+esp_random()%2001u;}
void resetShellBlink(){blinking=false;complete=false;hold=0;nextBlink=millis()+gap();}
void setShellBlinkFrozen(bool value){
  if(value==frozen)return;
  uint32_t now=millis();
  if(value)freezeAt=now;
  else {uint32_t duration=now-freezeAt;blinkStart+=duration;nextBlink+=duration;}
  frozen=value;
}
bool takeShellBlinkCycleComplete(){bool result=complete;complete=false;return result;}
void requestShellBlinkHold(uint32_t ms){
  if(!loaded||frozen||blinking||!ms)return;
  hold=ms;blinkStart=millis();blinking=true;
}
static float closure(uint32_t now) {
  if(frozen)now=freezeAt;
  if(!blinking){
    if(frozen||(int32_t)(now-nextBlink)<0)return 1;
    blinkStart=now;hold=0;blinking=true;
  }
  uint32_t age=now-blinkStart;
  if(age>=250u+hold){blinking=false;complete=true;nextBlink=now+gap();return 1;}
  blinkFrame=kami_expression::blink(age,hold);
  return blinkFrame.squash;
}
bool beginSession(TFT_eSPI& t){
  prepareMotionCache(t);prepareWakeCache(t);
  if(!loaded){loaded=true;frozen=false;offsetX=offsetY=0;setEmotion("neutral",true);resetShellBlink();
    Serial.printf("[eye-rig] ready presets=%u renderer=coverage-v1 sd=0\n",rig::kPresetCount);}
  return true;
}
bool beginSessionSingleFrame(TFT_eSPI& t){return beginSession(t);}
bool upgradeToFullSession(TFT_eSPI& t){return beginSession(t);}
void endSession(){homeSqueeze=0;loaded=false;frozen=false;blinking=false;hearts=false;offsetX=offsetY=0;previousTop=previousBottom=-1;previousExpression=false;}
bool isLoaded(){return loaded;}
// Compatibility: callers can use scalable drawing without allocation-failure fallback.
bool isProcedural(){return false;}
int numFrames(){return 3;}
void setEyeOffset(float x,float y){offsetX=x;offsetY=y;}
void setHomeMotion(float squeeze,bool /*contact*/){
  homeSqueeze=squeeze;
  if(squeeze>0){blinking=false;nextBlink=millis()+2200;}

}

static void bounds(TFT_eSPI& t,int yOff,int& x,int& y,int& w,int& h){
  w=min(kWidth,(int)t.width());h=min(kHeight,(int)t.height()-kSystemStatusBarH);
  x=(t.width()-w)/2;y=max(kSystemStatusBarH,min(kTop+yOff,(int)t.height()-h));
}
bool currentFrameBounds(TFT_eSPI& t,int* x,int* y,int* w,int* h){
  if(!loaded)return false;int bx,by,bw,bh;bounds(t,0,bx,by,bw,bh);
  if(x)*x=bx;if(y)*y=by;if(w)*w=bw;if(h)*h=bh;return true;
}
static void drawHearts(TFT_eSPI& t,uint32_t now,int top,int bottom){
  if(!hearts)return;
  uint32_t age=now-heartStart;if(age>2200){hearts=false;return;}
  for(int i=0;i<5;++i){
    int a=(int)age-i*140;if(a<0)continue;
    int x=48+i*43,y=bottom-8-a/22;if(y<top+8)continue;
    t.fillCircle(x-2,y-2,3,TFT_RED);t.fillCircle(x+2,y-2,3,TFT_RED);
    t.fillTriangle(x-5,y-1,x+5,y-1,x,y+5,TFT_RED);
  }
}
static bool paint(TFT_eSPI& t,float blink,int yOff,uint32_t now){
  if(!loaded)return false;sample(now);int x,y,w,h;bounds(t,yOff,x,y,w,h);
  float nominal=min(88.f,w/2.8f);
  if(!kami_motion::active(now)&&!strcmp(emotion,"neutral")){
    float ox=rig::clamp(offsetX,-26.5f,26.5f),oy=rig::clamp(offsetY,-23.25f,23.25f);
    uint32_t start=micros();
    if(previousExpression){t.fillRect(0,kSystemStatusBarH,t.width(),t.height()-kSystemStatusBarH,TFT_WHITE);hal::display::markDirty(kSystemStatusBarH,t.height()-kSystemStatusBarH);}
    int top=max(kSystemStatusBarH,(int)floorf(y+h*.5f+oy-kCacheH*.5f)-1);
    int bottom=min((int)t.height(),(int)ceilf(y+h*.5f+oy+kCacheH*.5f)+1);
    int first=previousTop<0?y:min(top,previousTop),last=previousBottom<0?y+h:max(bottom,previousBottom);
    if(motionCache){
      // Preserve shape as well as squash: a closed blink has squash=1,
      // but Shape::Squeeze. Treating that as open caused two flashes per blink.
      uint32_t age=kami_expression::blinkCacheMs(now-blinkStart,hold);
      int index=homeSqueeze>0?(int)lroundf(homeSqueeze*20):blinking?21+(int)(age/10):0;
      if(homeSqueeze==0&&!blinking&&blink<.999f)
        index=21+(int)lroundf(5*sqrtf(rig::clamp((1-blink)/.9f,0,1)));
      t.fillRect(x,first,w,last-first,TFT_WHITE);
      const bool swap=t.getSwapBytes();t.setSwapBytes(false);
      const uint16_t* pixels=framePixels(motionCache,motionOffsets,index,kCacheFrames,kCacheW*kCacheH);
      if(pixels)t.pushImage((int)lroundf(x+w*.5f+ox-kCacheW*.5f),(int)lroundf(y+h*.5f+oy-kCacheH*.5f),kCacheW,kCacheH,pixels);
      else if(homeSqueeze>0||!blinking)rig::drawHomeMorph(t,x,y,w,h,homeSqueeze,homeSqueeze>0?1:blink,nominal,ox,oy,first-y,last-y);
      else rig::drawExpression(t,x,y,w,h,current,blinkFrame,TFT_WHITE,nominal,rig::kHomeSpacing,ox,oy);
      t.setSwapBytes(swap);
    }else if(homeSqueeze>0||!blinking)
      rig::drawHomeMorph(t,x,y,w,h,homeSqueeze,homeSqueeze>0?1:blink,nominal,ox,oy,first-y,last-y);
    else {
      t.fillRect(x,first,w,last-first,TFT_WHITE);
      rig::drawExpression(t,x,y,w,h,current,blinkFrame,TFT_WHITE,nominal,rig::kHomeSpacing,ox,oy);
    }
    previousExpression=false;previousTop=top;previousBottom=bottom;++frames;
    lastRenderUs=micros()-start;maxRenderUs=max(lastRenderUs,maxRenderUs);
    hal::display::markDirty(first,last-first);return true;
  }

  auto expression=kami_motion::active(now)?kami_motion::frame(now):kami_expression::Frame{};
  if(!kami_motion::active(now)){expression.squash=blink;if(blinking)expression.shape=blinkFrame.shape;}
  if(expression.wakeMs>=0){
    uint32_t start=micros();paintWake(t,expression.wakeMs,yOff);++frames;
    lastRenderUs=micros()-start;maxRenderUs=max(lastRenderUs,maxRenderUs);return true;
  }
  if(expression.shape!=kami_expression::Shape::Open||kami_motion::active(now)){
    const uint32_t expressionStart=micros();
    // Decorative marks reach beyond the ordinary eye backing rectangle. Restore
    // their complete region on every event frame, including the recovery frame.
    t.fillRect(0,kSystemStatusBarH,t.width(),t.height()-kSystemStatusBarH,TFT_WHITE);
    if(expression.startleMix>=0)
      rig::drawExpression(t,0,kSystemStatusBarH,t.width(),t.height()-kSystemStatusBarH,current,expression,TFT_WHITE,nominal,rig::kHomeSpacing,0,-12);
    else rig::drawExpression(t,x,y,w,h,current,expression,TFT_WHITE,nominal,rig::kHomeSpacing);
    rig::drawExpressionMarks(t,0,kSystemStatusBarH,t.width(),t.height()-kSystemStatusBarH,
                            x+w*.5f,y+h*.5f,nominal,expression);
    previousExpression=true;previousTop=y;previousBottom=y+h;++frames;
    lastRenderUs=micros()-expressionStart;maxRenderUs=max(lastRenderUs,maxRenderUs);
    hal::display::markDirty(kSystemStatusBarH,t.height()-kSystemStatusBarH);return true;
  }
  if(previousExpression){t.fillRect(0,kSystemStatusBarH,t.width(),t.height()-kSystemStatusBarH,TFT_WHITE);
    hal::display::markDirty(kSystemStatusBarH,t.height()-kSystemStatusBarH);previousTop=-1;previousBottom=-1;previousExpression=false;}
  float ew=nominal*(1+(current.scale-1)*.4f);
  float eh=nominal*91.f/67.f*current.scale*blink;
  float pair=rig::centerSeparation(nominal,rig::kHomeSpacing)+ew;
  float ox=rig::clamp(offsetX,-fmaxf(0,(w-pair)*.5f-2),fmaxf(0,(w-pair)*.5f-2));
  float oy=rig::clamp(offsetY,-fmaxf(0,(h-eh)*.5f-2),fmaxf(0,(h-eh)*.5f-2));
  // Restore the union of old/new eye bands, not a 170px blank backing surface.
  // Leave the geometry centered in the full rect while clipping raster output.
  int inkTop=max(y,(int)floorf(y+h*.5f+oy-eh*.5f)-2);
  int inkBottom=min(y+h,(int)ceilf(y+h*.5f+oy+eh*.5f)+2);
  int dirtyTop=previousTop<0?y:min(inkTop,previousTop);
  int dirtyBottom=previousBottom<0?y+h:max(inkBottom,previousBottom);
  if(hearts){dirtyTop=y;dirtyBottom=y+h;inkTop=y;inkBottom=y+h;}
  dirtyTop=max(y,dirtyTop);dirtyBottom=min(y+h,dirtyBottom);
  uint32_t start=micros();
  rig::draw(t,x,y,w,h,current,blink,TFT_WHITE,nominal,ox,oy,false,255,dirtyTop-y,dirtyBottom-y,rig::kHomeSpacing);
  drawHearts(t,now,y,y+h);
  previousTop=inkTop;previousBottom=inkBottom;
  lastRenderUs=micros()-start;maxRenderUs=max(lastRenderUs,maxRenderUs);++frames;
  hal::display::markDirty(dirtyTop,dirtyBottom-dirtyTop);return true;
}
bool drawTravelEyes(TFT_eSPI& t,float avatar,float offsetX,float offsetY){
  if(!motionCache)return false;
  const float a=rig::clamp(avatar,0,1);
  const int frame=(int)lroundf(a*30);
  const float sampled=frame/30.f,nominal=88+(16.f/2.2f-88)*sampled;
  const float pair=rig::centerSeparation(nominal,.9f+.1f*sampled)+nominal;
  const int w=min(kCacheW,2*((int)ceilf(pair*.5f)+1));
  const int h=min(kCacheH,2*((int)ceilf(nominal*91.f/134.f)+1));
  const int x=(int)lroundf(140-102*a+offsetX-w*.5f),y=(int)lroundf(127-75*a+offsetY-h*.5f);
  const uint16_t* pixels=framePixels(motionCache,motionOffsets,47+frame,kCacheFrames,kCacheW*kCacheH);
  if(!pixels)return false;
  const uint16_t* src=pixels+(kCacheH-h)/2*kCacheW+(kCacheW-w)/2;
  const bool swap=t.getSwapBytes();t.setSwapBytes(false);
  for(int row=0;row<h;++row)t.pushImage(x,y+row,w,1,src+row*kCacheW);
  t.setSwapBytes(swap);return true;
}
bool drawThinkingEyes(TFT_eSPI& t,float settle,uint32_t now,bool onlyChanged){
  static int lastPose=-1,lastPhase=-1000;
  const float blink=closure(now);
  const int poseStep=(int)lroundf(rig::clamp(settle,0,1)*24);
  const int phase=blinking?(int)(kami_expression::blinkCacheMs(now-blinkStart,hold)/10):-1;
  if(onlyChanged && poseStep==lastPose && phase==lastPhase)return false;
  lastPose=poseStep;lastPhase=phase;
  const auto pose=rig::quietThinkingPose(settle);
  if(blinking && blinkFrame.shape!=kami_expression::Shape::Open)
    rig::drawExpression(t,14,42,252,170,pose,blinkFrame,TFT_WHITE,88,rig::kHomeSpacing);
  else rig::draw(t,14,42,252,170,pose,blink,TFT_WHITE,88,0,0,false,255,18,152,rig::kHomeSpacing);
  hal::display::markDirty(60,134);
  return true;
}
bool drawConversationEyes(TFT_eSPI& t,float x,float y,uint32_t now,bool onlyChanged){
  static int lastX=-1000,lastY=-1000,lastBlink=-1000;
  homeSqueeze=0;offsetX=x;offsetY=y;
  const float blink=closure(now);
  const int bx=(int)lroundf(48+x),by=(int)lroundf(61+y);
  const int phase=blinking?(int)(kami_expression::blinkCacheMs(now-blinkStart,hold)/10):-1;
  if(onlyChanged && motionCache && bx==lastX && by==lastY && phase==lastBlink)return false;
  lastX=bx;lastY=by;lastBlink=phase;
  return paint(t,blink,0,now);
}
bool drawFrame(TFT_eSPI& t,int frame,int yOff){
  if(frame<0||frame>=3)return false;
  return paint(t,frame==0?1:frame==1?.5f:.05f,yOff,millis());
}
bool serviceEyesShell(TFT_eSPI& t,int yOff){
  if(!loaded||frozen)return false;
  uint32_t now=millis();
  if(tapEnabled&&hal::imu::isReady()&&hal::imu::pollTap(now)){
    heartStart=now;hearts=true;requestShellBlinkHold(700);
  }
  return paint(t,closure(now),yOff,now);
}
bool serviceShellBlink(TFT_eSPI& t){return serviceEyesShell(t);}
bool advanceOpenEyeOverlay(TFT_eSPI& t,int yOff,uint32_t now){return paint(t,1,yOff,now);}
bool drawFrameToPanel(TFT_eSPI& t,int frame,int x,int y,int w,int h,uint16_t bg){
  if(!loaded||frame<0||frame>=3)return false;sample(millis());
  rig::draw(t,x,y,w,h,current,frame==0?1:frame==1?.5f:.05f,bg,88.f*w/kWidth,
            offsetX*w/kWidth,offsetY*h/kHeight,false,255,0,-1,rig::kHomeSpacing);
  hal::display::markDirty(y,h);return true;
}
bool drawMiniFrameToPanel(TFT_eSPI& t,int frame,int x,int y,int w,int h,uint16_t bg){
  if(frame<0||frame>=3)return false;
  rig::draw(t,x,y,w,h,current,frame==0?1:frame==1?.5f:.05f,bg);hal::display::markDirty(y,h);return true;
}
bool drawMiniFrame(TFT_eSprite& t,int frame,uint16_t bg){return drawMiniFrameToPanel(t,frame,0,0,t.width(),t.height(),bg);}
void setKamiEmotionTargets(const KamiEyeOcc& l,const KamiEyeOcc& r,float speed){
  rig::Pose p={{l.topY,l.topX,l.bottomY,l.bottomX,l.size},{r.topY,r.topX,r.bottomY,r.bottomX,r.size},current.scale};
  transition(p,speed);emotion="custom";
}
bool loadKamiEmotionFromJsonFile(const char* path){
  if(!path||!path[0])return false;File f=SD.open(path,FILE_READ);if(!f)return false;
  if(f.size()>6144){f.close();return false;}JsonDocument doc;auto error=deserializeJson(doc,f);f.close();if(error)return false;
  rig::Pose p=current;
  auto read=[](JsonObjectConst o,rig::Lid& lid){
    if(!o["topY"].isNull())lid.topY=o["topY"].as<float>();
    if(!o["topX"].isNull())lid.topX=o["topX"].as<float>();
    if(!o["bottomY"].isNull())lid.bottomY=o["bottomY"].as<float>();
    if(!o["bottomX"].isNull())lid.bottomX=o["bottomX"].as<float>();
    if(!o["size"].isNull())lid.size=o["size"].as<float>();
  };
  read(doc["left"],p.left);read(doc["right"],p.right);
  p.scale=doc["eyeScale"]|1.f;
  transition(p,doc["springSpeed"]|(doc["spring"]|.8f));emotion="custom";return true;
}
void beginThinkingEmotion(const char* path){if(!loadKamiEmotionFromJsonFile(path))setEmotion("thinking");}
void endThinkingEmotion(){setEmotion("neutral");setShellBlinkFrozen(false);}
void setTapHeartsEnabled(bool en){tapEnabled=en;}
bool getTapHeartsEnabled(){return tapEnabled;}
void setKamiAliveTune(const KamiAliveTune& t){alive=t;}
KamiAliveTune getKamiAliveTune(){return alive;}
void resetProceduralPaintState(int){}
bool takeEyeDrawChromeDirty(){return false;}
void fillEyeCanvasBackground(TFT_eSPI& t,int y,int w,int h){t.fillRect(0,y,w,h,TFT_WHITE);hal::display::markDirty(y,h);}
void fillBmpBottomLetterbox(TFT_eSPI&){}
void fillBmpBottomLetterboxAfterAdvance(TFT_eSPI&){}

void sleepTransition(TFT_eSPI& t){
  beginSession(t);offsetX=offsetY=0;frozen=false;blinking=false;setEmotion("neutral",true);
  uint32_t start=millis();
  kami_motion::trigger(kami_expression::Action::Sleep,start);
  while(millis()-start<kami_expression::kSleepCloseMs+kami_expression::kSleepPoseHoldMs){
    // One gentle close, then show the sleeping bowl and Zs before lights out.
    uint32_t now=millis();
    paint(t,1,0,now);hal::display::flush();core::watchdog::feed();delay(10);
  }
  Serial.println("[kami-motion] sleep bowl shown before backlight off");
}
bool handleDebugCommand(const char* line){
  if(strncmp(line,"eyes ",5))return false;
  const char* cmd=line+5;
  if(!strcmp(cmd,"list")){for(const auto& p:rig::kPresets)Serial.printf("[eye-rig] preset=%s\n",p.name);}
  else if(!strcmp(cmd,"status"))Serial.printf("[eye-rig] emotion=%s loaded=%u frames=%lu render_us=%lu max_us=%lu blink=%u ms=%lu flush_us=%lu home-motion=1 cache=%u squeeze=%.3f offset=%.2f,%.2f\n",emotion,loaded,(unsigned long)frames,(unsigned long)lastRenderUs,(unsigned long)maxRenderUs,blinking,(unsigned long)millis(),(unsigned long)hal::display::lastFlushUs(),motionCache?1:0,homeSqueeze,offsetX,offsetY);
  else if(!strcmp(cmd,"auto")){kami_motion::reset();setShellBlinkFrozen(false);setEmotion("neutral");resetShellBlink();}
  else if(!strcmp(cmd,"motion-reset")){kami_motion::reset();kami_motion::resetDetector();Serial.println("[kami-motion] detector reset, injected samples until eyes auto");}
  else if(!strncmp(cmd,"motion ",7)){
    long a,b,c;if(sscanf(cmd+7,"%ld %ld %ld",&a,&b,&c)==3){int32_t xyz[3]={(int32_t)a,(int32_t)b,(int32_t)c};kami_motion::inject(millis(),xyz);}
  }
  else if(!strncmp(cmd,"expression ",11)){
    char name[20];unsigned age=0;int fields=sscanf(cmd+11,"%19s %u",name,&age);kami_expression::Action action;
    if(fields>=1&&kami_expression::action(name,action)){
      if(fields==2)kami_motion::hold(action,age);else kami_motion::trigger(action,millis());
      frozen=false;blinking=false;
      Serial.printf("[kami-motion] expression=%s shape=%s held=%u\n",name,kami_expression::shapeName(kami_motion::frame(millis()).shape),fields==2);
    }else Serial.println("[kami-motion] unknown expression");
  }
  else if(!strcmp(cmd,"motion-status")){auto f=kami_motion::frame(millis());Serial.printf("[kami-motion] current=%s shape=%s imu=%u\n",kami_motion::currentName(millis()),kami_expression::shapeName(f.shape),hal::imu::isReady());}
  else if(!strcmp(cmd,"imu-stats"))kami_motion::printStats(millis());
  else if(!strcmp(cmd,"blink")){requestShellBlinkHold(1);}
  else if(!strcmp(cmd,"freeze")){setShellBlinkFrozen(true);}
  else if(!strcmp(cmd,"play")){setShellBlinkFrozen(false);}
  else if(!strncmp(cmd,"pose ",5)){
    if(setEmotion(cmd+5,true)){blinking=false;nextBlink=millis()+gap();paint(hal::display::tft(),1,0,millis());Serial.printf("[eye-rig] pose=%s\n",emotion);}
    else Serial.println("[eye-rig] unknown preset");
  } else Serial.println("[eye-rig] eyes list|status|pose NAME|auto|blink|freeze|play");
  return true;
}
}} // namespace ui::home_eyes
