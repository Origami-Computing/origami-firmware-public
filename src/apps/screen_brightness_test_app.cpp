#ifdef ORIGAMI_DEV_SERIAL_INPUT
#include "apps/screen_brightness_test_app.h"
#include "app.h"
#include "scene/screen.h"
#include "nav_handler.h"
#include "hal/hal_display.h"
#include "services/screen_brightness.h"
#include "ui/nav_controls.h"
#include <stdio.h>

namespace apps { namespace screen_brightness_test {
namespace {
extern App app;
bool active=false, redraw=true, swapped=false;
int targetPercent=100, drivePercent=-1, previousPercent=100;
uint16_t candidate=0xFFFF;
uint32_t expires=0;
const uint16_t background=0;

void suspend() { services::screen_brightness::set(previousPercent); }
void resume() { services::screen_brightness::set(100); redraw=true; hal::display::markAllDirty(); }
void enter() { active=true; previousPercent=services::screen_brightness::percent(); resume(); }
void exit() { suspend(); active=false; }
void enterView() { redraw=true; hal::display::markAllDirty(); }
void tick(uint32_t now) { if(static_cast<int32_t>(now-expires)>=0) NavHandler::instance().pop(); }
void input(hal::input::InputEvent ev) {
  if(ui::nav::isBackTap(ev)||ui::nav::isEnterTap(ev)) NavHandler::instance().pop();
}
bool chord() { NavHandler::instance().pop(); return true; }
void draw(TFT_eSprite& canvas) {
  if(!redraw)return;
  redraw=false;
  canvas.fillSprite(0);
  const float t=(targetPercent+16.0f)/116.0f;
  const float luminance=targetPercent<=8?targetPercent/903.2963f:t*t*t;
  const unsigned coverage=static_cast<unsigned>(luminance*1024+.5f);
  const int referenceX=swapped?160:24, candidateX=swapped?24:160;
  uint16_t row[96];
  for(unsigned y=0;y<96;++y) {
    for(unsigned x=0;x<96;++x) {
      unsigned rank=0;
      for(unsigned bit=0;bit<5;++bit) {
        const unsigned xb=(x>>bit)&1, yb=(y>>bit)&1;
        rank=(rank<<2)|((xb^yb)<<1)|yb;
      }
      row[x]=rank<coverage?0xFFFF:0;
    }
    canvas.pushImage(referenceX,72+y,96,1,row);
  }
  canvas.fillRect(candidateX,72,96,96,candidate);
  canvas.setTextFont(1);canvas.setTextSize(2);canvas.setTextColor(0xFFFF,0);
  canvas.setTextDatum(MC_DATUM);
  char label[32];snprintf(label,sizeof(label),"Brightness test: %d%%",targetPercent);
  canvas.drawString(label,140,32);
  canvas.setTextSize(1);
  canvas.drawString("Reference",referenceX+48,188);
  canvas.drawString("Scaled",candidateX+48,188);
  canvas.drawString("A/C: close / 15s timeout",140,216);
  canvas.setTextDatum(TL_DATUM);
  hal::display::markAllDirty();
}
AppPresentation presentation() { return {true,true,AppPresentation::StatusBar::Hidden}; }
core::health::RuntimePolicy runtime() { return {true,false,false,true}; }
const ::scene::Screen screen={"Optical brightness test",enterView,nullptr,tick,input,chord,draw,
    "optical",nullptr,&background};
const ::scene::Screen* const screens[]={&screen};
App app={"Brightness optical test",&screen,nullptr,nullptr,exit,"brightness-optical",
    "src/apps/screen_brightness_test_app.cpp",screens,1,presentation,runtime,enter,resume,nullptr,suspend};
}

bool show(int target,int drive,bool swap) {
  if(target<0||target>100||drive < -1||drive>100)return false;
  if(active && NavHandler::instance().top()!=&app)return false;
  targetPercent=target;drivePercent=drive;swapped=swap;
  if(drive>=0) {
    const PixelScale565 raw(drive);
    candidate=raw.map(0xFFFF);
  } else {
    const int previous=services::screen_brightness::percent();
    services::screen_brightness::set(target);
    candidate=hal::display::brightnessColor(0xFFFF);
    services::screen_brightness::set(previous);
  }
  expires=millis()+15000u;
  redraw=true;
  if(!active)NavHandler::instance().push(&app);
  hal::display::markAllDirty();
  Serial.printf("[brightness-pattern] target=%d drive=%d swapped=%u candidate=0x%04X\n",
      targetPercent,drivePercent,swapped?1u:0u,candidate);
  return true;
}
bool close() {
  if(!active)return true;
  if(NavHandler::instance().top()!=&app)return false;
  NavHandler::instance().pop();
  return true;
}
} }
#endif
