#include "sfx/alarm_catalog.h"
#include "services/vibration.h"
/**
 * @file timer_app.cpp
 * @brief Declarative, automation-backed countdown timer.
 */

#include "apps/timer_app.h"

#include <Arduino.h>
#include <stdio.h>
#include <time.h>

#include "app.h"
#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "services/automation.h"
#include "core/alert_vibration.h"
#include "services/countdown.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "services.h"
#include "sfx/sfx_lib.h"
#include "ui/draw.h"
#include "ui/playback_status.h"
#include "ui/timer_controls.h"
#include "ui/nav_controls.h"
#include "ui/scene.h"
#include "ui/edit_field.h"
#include "ui/morning_style.h"
#include "ui/source_layout.h"

namespace apps {
namespace timer {

extern const ::scene::Screen kScreenTimerSet;
extern const ::scene::Screen kScreenTimerRun;
extern const ::scene::Screen kScreenTimerDone;

namespace {

using services::countdown::TimerState;
using services::countdown::clampDuration;
static constexpr uint32_t kDefaultDurationSec = 5u * 60u;

enum class ScreenMode : uint8_t { Set, Run, Done };
enum class EditField : uint8_t { Minutes, Seconds, Sound, Vibration, Start };

static bool s_active = false;
static bool s_adjusting = false;
static bool s_blink = true;
static uint32_t s_blinkMs = 0;
static bool s_needRedraw = false;
static ScreenMode s_mode = ScreenMode::Set;
static TimerState s_state = TimerState::Idle;
static EditField s_editField = EditField::Minutes;

static uint8_t s_setMinutes = 5;
static uint8_t s_setSeconds = 0;
static uint32_t s_cycleDuration=300,s_clockMinute=UINT32_MAX;
static uint32_t s_durationSec = kDefaultDurationSec;
static uint32_t s_startedAt = 0;
static uint32_t s_fireAt = 0;
static uint32_t s_remainingSec = kDefaultDurationSec;
static uint32_t s_lastDrawRemainingSec = UINT32_MAX;
static services::automation::AlertSound s_sound = services::automation::AlertSound::Classic;
static uint8_t s_vibration=0;
static char s_status[40] = {};
static uint32_t s_seenRevision=0;

static void changeSound(int delta) {
  int value = (int)s_sound + delta;
  constexpr int kSoundCount = 4;
  while (value < 0) value += kSoundCount;
  while (value >= kSoundCount) value -= kSoundCount;
  s_sound = (services::automation::AlertSound)value;
}

static void setStatus(const char* text) {
  snprintf(s_status, sizeof(s_status), "%s", text ? text : "");
}

static void markRedraw() {
  s_needRedraw = true;
  hal::display::markAllDirty();
}

static void saveState() {
  services::countdown::save({s_state,s_durationSec,s_startedAt,s_fireAt,
                            s_remainingSec,s_sound,s_vibration});
  s_seenRevision=services::countdown::revision();
}

static void syncSetFieldsFromDuration() {
  const uint32_t duration = clampDuration(s_durationSec);
  s_setMinutes = (uint8_t)(duration / 60u);
  s_setSeconds = (uint8_t)(duration % 60u);
}

static uint32_t selectedDurationSec() {
  return (uint32_t)s_setMinutes * 60u + (uint32_t)s_setSeconds;
}

static void enforceSetMinimum() {
  if (selectedDurationSec() == 0) s_setSeconds = 5;
}

static void loadStateForUi() {
  const auto record = services::countdown::snapshot();
  s_seenRevision=services::countdown::revision();
  s_state=record.state; s_durationSec=record.duration;
  s_cycleDuration=record.cycleDuration?record.cycleDuration:record.duration;
  s_startedAt=record.startedAt; s_fireAt=record.fireAt;
  s_remainingSec=record.remaining; s_sound=record.sound; s_vibration=record.vibration;
  s_status[0] = '\0';

  if (s_state == TimerState::Idle || s_state == TimerState::Done) {
    syncSetFieldsFromDuration();
  }
}

static bool acceptTransition(services::countdown::Result result) {
  using Result=services::countdown::Result;
  loadStateForUi();
  if(result==Result::Ok) return true;
  setStatus(result==Result::ClockUnsynced ? "Clock sync required" :
            result==Result::QueueFull ? "Timer queue full" :
            result==Result::StorageError ? "Timer save failed" : "Timer unavailable");
  sfx::play(sfx::kSfxAlarmError);markRedraw();return false;
}
static void resetToSet() {
  if(!acceptTransition(services::countdown::reset())) return;
  s_editField=EditField::Minutes;
  syncSetFieldsFromDuration();
  ::scene::switchTo(&app,&kScreenTimerSet, ::scene::ScreenTransition::Back);
}
static bool startTimer(uint32_t durationSec) {
  services::vibration::stopPreview();
  if(!acceptTransition(services::countdown::start(clampDuration(durationSec),s_sound,s_vibration)))return false;
  sfx::play(sfx::kSfxAlarmOn);
  ::scene::switchTo(&app,&kScreenTimerRun);return true;
}
static void pauseTimer() {
  if(!acceptTransition(services::countdown::pause()))return;
  sfx::play(sfx::kSfxAlarmOff);
  if(s_state==TimerState::Done)::scene::switchTo(&app,&kScreenTimerDone);
  else markRedraw();
}
static void resumeTimer() {
  if(!acceptTransition(services::countdown::resume()))return;
  sfx::play(sfx::kSfxAlarmOn);markRedraw();
}
static void updateRunningFromClock() {
  // External callers and the native view observe one service state machine.
  const auto record=services::countdown::snapshot();
  if(s_seenRevision!=services::countdown::revision() || record.state!=s_state) {
    loadStateForUi();
    const auto* target=s_state==TimerState::Done ? &kScreenTimerDone :
      (s_state==TimerState::Running || s_state==TimerState::Paused) ? &kScreenTimerRun : &kScreenTimerSet;
    if(app.currentScreen!=target)::scene::switchTo(&app,target);
    else markRedraw();
  }
  if(s_state==TimerState::Running)s_remainingSec=record.remaining;
}


static void drawScene(TFT_eSprite& canvas,
                      void (*build)(ui::scene::Scene&, int16_t, int16_t)) {
  ui::scene::Scene& scene = ui::scene::scratchScene();
  build(scene, canvas.width(), canvas.height());
  scene.render(canvas);
  hal::display::markAllDirty();
}

static void buildSetScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  const bool physicalSurface = width==ui::safe_area::kWidth && height==ui::safe_area::kHeight;
  scene.clear();
  ui::scene::Builder builder(scene,width,height); builder.clearContent(kUiBg);
  char minutes[8],seconds[8],sound[48];
  snprintf(minutes,sizeof(minutes),"%02u",s_setMinutes);
  snprintf(seconds,sizeof(seconds),"%02u",s_setSeconds);
  snprintf(sound,sizeof(sound),"Sound: %s",sfx::alarms::preset((uint8_t)s_sound).name);
  scene.addLabel("timer.units",{0,42,width,20},"Minutes : Seconds",kUiFg,kUiBg,2,ui::scene::TextAlign::MiddleCenter,2);
  const int x=width/2-72;
  ui::editField(scene,physicalSurface,"timer.minutes",{(int16_t)x,70,60,44},minutes,s_editField==EditField::Minutes,s_adjusting,s_blink,4);
  scene.addLabel("timer.colon",{(int16_t)(x+60),70,24,44},":",kUiFg,kUiBg,4,ui::scene::TextAlign::MiddleCenter,2);
  ui::editField(scene,physicalSurface,"timer.seconds",{(int16_t)(x+84),70,60,44},seconds,s_editField==EditField::Seconds,s_adjusting,s_blink,4);
  ui::editField(scene,physicalSurface,"timer.sound",{14,132,(int16_t)(width-28),32},sound,s_editField==EditField::Sound,s_adjusting,s_blink);
  char vibration[40];snprintf(vibration,sizeof(vibration),"Buzz: %s",core::alert_vibration::name(s_vibration));
  ui::editField(scene,physicalSurface,"timer.vibration",{14,168,(int16_t)(width-28),28},vibration,s_editField==EditField::Vibration,s_adjusting,s_blink,1);
  ui::editField(scene,physicalSurface,"timer.start",{14,200,(int16_t)(width-28),28},"Start",s_editField==EditField::Start,false,true);
  if(s_editField==EditField::Vibration)scene.addLabel("timer.vibration.test",{16,229,(int16_t)(width-32),10},"B: Test / stop",kUiFg,kUiBg,1,ui::scene::TextAlign::MiddleCenter,2);
    if(s_status[0]) scene.addLabel("timer.status",{14,114,(int16_t)(width-28),16},s_status,kUiFg,kUiBg,1,ui::scene::TextAlign::MiddleCenter,2);
}

static void buildRunScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  ui::scene::Builder builder(scene, width, height);
  scene.addRect("timer.background",{0,0,width,height},kUiBg);
  char remaining[16];
  ui::formatDurationMs((s_state==TimerState::Idle?s_durationSec:s_remainingSec)*1000u,remaining,sizeof(remaining));
  scene.addLabel("timer.phase",{0,52,width,24},"Timer",kUiFg,kUiBg,2,ui::scene::TextAlign::MiddleCenter,2);
  scene.addLabel("timer.count",{0,90,width,52},remaining,kUiFg,kUiBg,6,ui::scene::TextAlign::MiddleCenter,2);
  const uint32_t elapsed=s_state==TimerState::Idle?0:(s_cycleDuration>s_remainingSec?s_cycleDuration-s_remainingSec:0);
  scene.addProgressBar("timer.bar",{24,154,(int16_t)(width-48),12},elapsed,s_cycleDuration?s_cycleDuration:1,kUiPanel,kUiAccent,3,2);

}

static void buildDoneScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  ui::scene::Builder builder(scene, width, height);
  builder.clearContent(kUiBg);
  ui::kami_message::message(scene,width,height,"timer.message","Time's up!");
  ui::morning_style::footer(scene,width,height,"C: Reset",ui::morning_style::Icon::Retry,"A: Back",ui::morning_style::Icon::Cancel);
  ui::scene::applySourceLayout(scene, "timer", "done");
}

}  // namespace

void enter() {
  if (s_active) return;
  s_active = true;
  loadStateForUi();
  s_lastDrawRemainingSec = UINT32_MAX;
  s_needRedraw = true;
  hal::display::markAllDirty();
}

void exitApp() {
  services::vibration::stopPreview();
  s_active = false;
}

void tick(uint32_t now) {
  if(s_mode==ScreenMode::Set && s_adjusting && now-s_blinkMs>=450u) {
    s_blinkMs=now; s_blink=!s_blink; markRedraw();
  }
  updateRunningFromClock();
  if (!s_active || s_mode != ScreenMode::Run) return;

  const uint32_t minute=(uint32_t)time(nullptr)/60;
  if(s_clockMinute!=minute){s_clockMinute=minute;markRedraw();}
  if (s_remainingSec != s_lastDrawRemainingSec) {
    s_lastDrawRemainingSec = s_remainingSec;
    markRedraw();
  }
}

void onInput(hal::input::InputEvent event) {
  using EventType = hal::input::EventType;
  if (!s_active) return;

  if (s_mode == ScreenMode::Set) {
    if(s_editField==EditField::Vibration && event.type==EventType::ButtonB_Tap) {
      if(services::vibration::previewing())services::vibration::stopPreview();
      else services::vibration::preview(s_vibration);
      return;
    }
    if (event.type == EventType::Scroll && event.delta != 0) {
      sfx::play(sfx::kSfxAlarmNavigate);
      if(!s_adjusting) {
        s_editField=(EditField)(((int)s_editField+event.delta%5+5)%5);
        markRedraw(); return;
      }
      if (s_editField == EditField::Vibration) {
        s_vibration=(s_vibration+event.delta%core::alert_vibration::kCount+core::alert_vibration::kCount)%core::alert_vibration::kCount;
        services::vibration::preview(s_vibration);saveState();markRedraw();return;
      }
      if (s_editField == EditField::Minutes) {
        int minutes = (int)s_setMinutes + event.delta;
        if (minutes < 0) minutes = 0;
        if (minutes > 99) minutes = 99;
        s_setMinutes = (uint8_t)minutes;
      } else {
        if (s_editField == EditField::Sound) {
          changeSound(event.delta);
          saveState();
          sfx::alarms::preview((uint8_t)s_sound);
          markRedraw();
          return;
        }
        int seconds = (int)s_setSeconds + event.delta * 5;
        while (seconds < 0) seconds += 60;
        while (seconds >= 60) seconds -= 60;
        s_setSeconds = (uint8_t)seconds;
      }
      enforceSetMinimum();
      s_durationSec = selectedDurationSec();
      s_remainingSec = s_durationSec;
      saveState();
      sfx::play(sfx::kSfxAlarmNavigate);
      markRedraw();
      return;
    }
    if (ui::nav::isEnterTap(event)) {
      if(s_editField==EditField::Start) startTimer(selectedDurationSec());
      else { s_adjusting=!s_adjusting; s_blink=true; s_blinkMs=millis(); if(s_adjusting && s_editField==EditField::Vibration)services::vibration::preview(s_vibration); markRedraw(); }
      return;
    }
    if (ui::nav::isBackTap(event)) {
      if(s_adjusting) { s_adjusting=false; markRedraw(); return; }
      NavHandler::instance().pop();
      return;
    }
    return;
  }

  if (s_mode == ScreenMode::Run) {
    if (event.type == EventType::ButtonA_Tap) { NavHandler::instance().popToRoot(); return; }
    if (event.type == EventType::ButtonC_Tap) {
      if (s_state == TimerState::Idle) startTimer(s_durationSec);
      else if (s_state == TimerState::Paused) resumeTimer();
      else pauseTimer();
      return;
    }
    if (event.type == EventType::ButtonB_Tap) {
      if(acceptTransition(services::countdown::reset())) markRedraw();
      return;
    }
    return;
  }

  if (s_mode == ScreenMode::Done) {
    if(ui::nav::isBackTap(event)) { NavHandler::instance().popToRoot(); return; }
    if (event.type == EventType::ButtonC_Tap) {
      startTimer(s_durationSec);
    }
  }
}

void onDraw(TFT_eSprite& canvas) {
  if (!s_active || !s_needRedraw) return;
  s_needRedraw = false;

  if (s_mode == ScreenMode::Run) {
    drawScene(canvas, buildRunScene);
    ui::timer_controls::back(canvas);ui::timer_controls::clock(canvas);
    ui::timer_controls::stop(canvas,240,16,s_state==TimerState::Running);
    ui::timer_controls::playback(canvas,s_state==TimerState::Running);
  }
  else if (s_mode == ScreenMode::Done) drawScene(canvas, buildDoneScene);
  else drawScene(canvas, buildSetScene);
}

static void screenTick(uint32_t now) {
  tick(now);
}

static void enterSet() {
  if (s_state == TimerState::Running || s_state == TimerState::Paused) {
    ::scene::switchTo(&app, &kScreenTimerRun);
    return;
  }
  if (s_state == TimerState::Done) {
    ::scene::switchTo(&app, &kScreenTimerDone);
    return;
  }
  s_mode = ScreenMode::Set;
  s_adjusting=false;
  markRedraw();
}

static void enterRun() {
  s_mode = ScreenMode::Run;
  s_lastDrawRemainingSec = UINT32_MAX;
  markRedraw();
}

static void enterDone() {
  s_mode = ScreenMode::Done;
  markRedraw();
}

const ::scene::Screen kScreenTimerSet = {
    "Timer",
    enterSet,
    nullptr,
    screenTick,
    onInput,
    nullptr,
    onDraw,
    "set",
    buildSetScene,
};

const ::scene::Screen kScreenTimerRun = {
    "Timer",
    enterRun,
    nullptr,
    screenTick,
    onInput,
    nullptr,
    onDraw,
    "run",
    buildRunScene,
};

const ::scene::Screen kScreenTimerDone = {
    "Timer",
    enterDone,
    nullptr,
    screenTick,
    onInput,
    nullptr,
    onDraw,
    "done",
    buildDoneScene,
};

static const ::scene::Screen* const kScreens[] = {
    &kScreenTimerSet,
    &kScreenTimerRun,
    &kScreenTimerDone,
};

static AppPresentation presentationPolicy() {
  return s_mode==ScreenMode::Run ? AppPresentation{true,false,AppPresentation::StatusBar::Hidden} : AppPresentation{};
}

App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Timer", "timer", &kScreenTimerSet, enter, enter, exitApp,
                            "src/apps/timer_app.cpp", kScreens, presentationPolicy, nullptr);

}  // namespace timer
}  // namespace apps
