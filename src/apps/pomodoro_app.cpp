#include "services/vibration.h"
/** Focus/break timer using the shared time-editor and alarm sounds. */
#ifdef MENU_APP
#include "apps/pomodoro_app.h"
#include <Arduino.h>
#include "app.h"
#include "services/pomodoro_settings.h"
#include "services/automation.h"
#include "core/alert_vibration.h"
#include "hal/hal_display.h"
#include "hal/hal_io.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "services/interval_feedback.h"
#include "ui/edit_field.h"
#include "ui/playback_status.h"
#include "ui/morning_style.h"
#include "ui/nav_controls.h"
#include "ui/pomodoro_view.h"
#include <time.h>

namespace apps { namespace pomodoro {
namespace {
enum class Mode { Setup, Running, Paused, Complete, Settings };
enum Field { Focus, Break, Sound, Vibration, FieldCount };
Mode s_mode=Mode::Setup;
Field s_field=Focus;
Mode s_returnMode=Mode::Setup;
uint32_t s_animationAt=0,s_lastAnimation=0;
void setMode(Mode mode){s_mode=mode;s_animationAt=millis();}
bool s_active=false, s_dirty=true, s_editing=false, s_blink=true, s_break=false;
services::pomodoro::Settings s_settings;
uint32_t s_remaining=1500, s_duration=1500, s_tickMs=0, s_blinkMs=0;
services::alerts::IntervalFeedback s_feedback(services::vibration::Owner::Pomodoro);

void redraw() { s_dirty=true; hal::display::markAllDirty(); }
void stopFeedback() { s_feedback.stop(); }
void beginInterval(bool isBreak) {
  services::vibration::stopPreview();
  stopFeedback();
  s_break=isBreak;
  s_duration=s_remaining=60u*(isBreak?s_settings.breakMinutes:s_settings.focusMinutes);
  s_tickMs=millis(); setMode(Mode::Running); redraw();
}
void buildPomodoroScene(ui::scene::Scene& scene,int16_t,int16_t) {
  pomodoro_view::build(scene,(int)s_mode,(int)s_field,s_editing,s_blink,s_settings.focusMinutes,s_settings.breakMinutes,s_settings.sound,s_settings.vibration,s_break,s_remaining,s_duration);
}

}
static void prepareWorkflow() {
  if(s_active) return;
  s_active=true; setMode(Mode::Setup);s_break=false; s_editing=false; s_field=Focus;
  s_settings.load();
  s_duration=s_remaining=60u*s_settings.focusMinutes;
}
void exitApp() { stopFeedback(); services::vibration::stopPreview(); s_settings.save(); s_active=false; s_mode=Mode::Setup; }
bool isActive() { return s_active; }
bool isAlerting() { return s_active && s_mode==Mode::Complete && NavHandler::instance().top()==&app; }
void serviceFeedback(uint32_t now) {
  const bool audible=s_active && s_mode==Mode::Complete &&
    NavHandler::instance().top()==&app && !NavHandler::instance().systemScreenActive() &&
    !services::automation::wantsInputCapture();
  if(!audible) { if(s_feedback.active()) stopFeedback(); return; }
  if(!s_feedback.active()) s_feedback.start(s_settings.sound,s_settings.vibration,now);
  s_feedback.poll(now);
}
void tick(uint32_t now) {
  if(!s_active) return;
  if(now-s_lastAnimation>=80u){s_lastAnimation=now;redraw();}
  if(s_editing && now-s_blinkMs>=450u) { s_blinkMs=now; s_blink=!s_blink; redraw(); }
  if(s_mode!=Mode::Running && !(s_mode==Mode::Settings&&s_returnMode==Mode::Running)) return;
  // Input can start/resume after the UI loop sampled now. Do not treat that
  // few-millisecond negative delta as a full uint32 wrap and finish instantly.
  if ((int32_t)(now-s_tickMs)<0) return;
  const uint32_t seconds=(now-s_tickMs)/1000u;
  if(!seconds) return;
  s_tickMs+=seconds*1000u;
  if(seconds>=s_remaining) {
    s_remaining=0; s_editing=false;setMode(Mode::Complete); redraw();
    Serial.printf("[pomodoro] %s complete; awaiting acknowledgement\n",s_break?"break":"focus");
  }
  else { s_remaining-=seconds; redraw(); }
}
bool onChord() {
  if(s_mode!=Mode::Settings)return false;
  s_settings.save();services::vibration::stopPreview();s_editing=false;setMode(s_returnMode);
  if(s_mode==Mode::Setup)s_duration=s_remaining=60u*(s_break?s_settings.breakMinutes:s_settings.focusMinutes);
  redraw();return true;
}
void onInput(hal::input::InputEvent ev) {
  using ET=hal::input::EventType;
  if(s_mode==Mode::Settings){
    if(ev.type==ET::Scroll&&ev.delta){
      if(!s_editing)s_field=(Field)(((int)s_field+ev.delta%FieldCount+FieldCount)%FieldCount);
      else if(s_field==Focus)s_settings.focusMinutes=constrain((int)s_settings.focusMinutes+ev.delta,1,99);
      else if(s_field==Break)s_settings.breakMinutes=constrain((int)s_settings.breakMinutes+ev.delta,1,99);
      else if(s_field==Sound){s_settings.sound=(s_settings.sound+ev.delta%4+4)%4;services::alerts::IntervalFeedback::previewSound(s_settings.sound);}
      else if(s_field==Vibration){s_settings.vibration=(s_settings.vibration+ev.delta%core::alert_vibration::kCount+core::alert_vibration::kCount)%core::alert_vibration::kCount;services::vibration::preview(s_settings.vibration);}
      redraw();
    }else if(ev.type==ET::ButtonC_Tap){s_editing=!s_editing;s_blink=true;s_blinkMs=millis();if(!s_editing){s_settings.save();services::vibration::stopPreview();}redraw();}
    return;
  }
  if(ev.type==ET::ButtonA_Tap){
    stopFeedback();s_break=!s_break;s_duration=s_remaining=60u*(s_break?s_settings.breakMinutes:s_settings.focusMinutes);setMode(Mode::Setup);
  }else if(ev.type==ET::ButtonB_Tap){
    if(s_mode==Mode::Running){tick(millis());if(s_mode==Mode::Running)setMode(Mode::Paused);}
    else if(s_mode==Mode::Paused){s_tickMs=millis();setMode(Mode::Running);}
    else beginInterval(s_mode==Mode::Complete?!s_break:s_break);
  }else if(ev.type==ET::ButtonC_Tap){
    stopFeedback();s_returnMode=s_mode;setMode(Mode::Settings);s_field=Focus;s_editing=false;
  }else return;
  redraw();
}
#ifdef ORIGAMI_DEV_SERIAL_INPUT
void printStatus() {
  const char* modes[]={"ready","running","paused","complete","settings"};
  uint8_t p0=0,p1=0,p2=0;
  const bool read=hal::io::readExpanderInputs(&p0,&p1,&p2);
  Serial.printf("[pomodoro] active=%u mode=%s section=%s remaining=%lu duration=%lu focus=%u break=%u sound=%u feedback=%u repeat=%u motor=%d\n",
    s_active,modes[(unsigned)s_mode],s_break?"break":"focus",(unsigned long)s_remaining,
    (unsigned long)s_duration,s_settings.focusMinutes,s_settings.breakMinutes,s_settings.sound,s_feedback.active(),s_feedback.repeating(),
    read?int((p0>>IOX_VIBRATION_MOTOR)&1):-1);
}
bool shortenForTest(uint32_t seconds) {
  if(!s_active || s_mode!=Mode::Running || seconds<1 || seconds>10) return false;
  s_remaining=seconds; s_tickMs=millis(); redraw(); return true;
}
#endif
void onDraw(TFT_eSprite& canvas) {
  if(!s_active || !s_dirty) return;
  s_dirty=false;
  auto& scene=ui::scene::scratchScene();
  buildPomodoroScene(scene,canvas.width(),canvas.height()); scene.render(canvas);
  char clock[8]="--:--";struct tm local{};
  if(getLocalTime(&local,0))snprintf(clock,sizeof(clock),"%02d:%02d",local.tm_hour,local.tm_min);
  pomodoro_view::finish(canvas,(int)s_mode,s_break,millis()-s_animationAt,clock);
  hal::display::markAllDirty();
}
static const ::scene::Screen kScreenPomodoro={
  "Pomodoro",redraw,nullptr,tick,onInput,onChord,onDraw,"timer",buildPomodoroScene};
static const ::scene::Screen* const kScreens[]={&kScreenPomodoro};
static AppPresentation presentationPolicy() {
    return {true, false, AppPresentation::StatusBar::Hidden};
}
static core::health::RuntimePolicy runtimePolicy() {
    return core::health::whileBusy(isActive());
}
App app = SCENE_APP_CATALOG_LIFECYCLE_POLICIES("Pomodoro","pomodoro",&kScreenPomodoro,prepareWorkflow,exitApp,"src/apps/pomodoro_app.cpp",kScreens, presentationPolicy, runtimePolicy);
}}
#endif
