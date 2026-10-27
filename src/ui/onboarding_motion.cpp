#include "ui/onboarding_motion.h"
#include "ui/home_eyes.h"
#include "ui/raster.h"
#include "ui/onboarding_approved_view.h"
#include "hal/hal_display.h"
#include "core/watchdog.h"
#include <esp_heap_caps.h>
#include <cstring>
#include <cmath>
namespace ui {
namespace {uint16_t swap(uint16_t p){return (p>>8)|(p<<8);}float ease(float t){t=t<0?0:t>1?1:t;return t*t*(3-2*t);}}
OnboardingMotion::OnboardingMotion():frame_(&hal::display::directTft()){}
OnboardingMotion::~OnboardingMotion(){close();}
void OnboardingMotion::close(){frame_.deleteSprite();heap_caps_free(wheel_);wheel_=nullptr;}
bool OnboardingMotion::prepareOutro(){close();frame_.setColorDepth(16);return frame_.createSprite(280,240)!=nullptr;}
bool OnboardingMotion::prepareWheel(){
 close();frame_.setColorDepth(16);
 wheel_=static_cast<uint16_t*>(heap_caps_malloc(onboarding_approved::kWheelFrames*24*24*2,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
 if(!wheel_||!frame_.createSprite(24,24)){close();return false;}
 for(unsigned i=0;i<onboarding_approved::kWheelFrames;++i){
  frame_.fillSprite(0xffff);
  drawDialIcon(frame_,12,12,.6f,0xffff,onboarding_intro::kRed,onboarding_approved::wheelAngle(i));
  memcpy(wheel_+i*24*24,frame_.getPointer(),24*24*2);core::watchdog::feed();
 }
 frame_.deleteSprite();return true;
}
bool OnboardingMotion::drawWheel(TFT_eSprite& target,unsigned elapsed){
 if(!wheel_)return false;
 const bool previous=target.getSwapBytes();target.setSwapBytes(false);
 target.pushImage(242,100,24,24,wheel_+onboarding_approved::wheelFrame(elapsed)*24*24);
 target.setSwapBytes(previous);return true;
}
bool OnboardingMotion::drawOutro(TFT_eSprite& c,TFT_eSprite& last,unsigned ms,SystemStatusBar& chrome){
 if(!frame_.created()||!last.created())return false;
 if(ms<1200){last.pushToSprite(&c,0,0);return true;}
 home_eyes::drawOutroWave(frame_,ms<1800?0:ms<3980?ms-1800:2180);
 chrome.service(millis());chrome.paintGlyphOverlay(frame_,0xffff);
 if(ms>=1800){frame_.pushToSprite(&c,0,0);return true;}
 const int a=int(lroundf(15*ease((ms-1200)/600.f)));
 auto* dst=static_cast<uint16_t*>(c.getPointer());
 auto* before=static_cast<const uint16_t*>(last.getPointer());auto* after=static_cast<const uint16_t*>(frame_.getPointer());
 for(int i=0;i<280*240;++i)dst[i]=swap(raster::blend(swap(after[i]),swap(before[i]),a));
 return true;
}
}
