#include "sfx/alarm_catalog.h"
#include "ui/automation_alert.h"
#include "services/background_network.h"

#include <Arduino.h>
#include <string.h>
#include <math.h>

#include "config/ui_theme.h"
#include "services/automation.h"
#include "hal/hal_display.h"
#include "sfx/sound_player.h"
#include "sfx/sfx_lib.h"
#include "ui/scene.h"
#include "ui/morning_eyes.h"
#include "ui/eye_rig.h"
#include "ui/timer_controls.h"
#include "services/countdown.h"
#include "core/menu_config.h"
#include "ui/morning_style.h"

namespace ui {
namespace {

static sfx::SoundPlayer s_ringSound;
static bool s_needRedraw = false;
using MorningPhase = core::morning_alarm::Phase;
static MorningPhase s_lastPhase = MorningPhase::Idle;
static bool s_wasRinging = false;
static services::automation::AlertSound s_lastSound = services::automation::AlertSound::Classic;
static uint32_t s_lastFrameMs = 0;
static uint32_t s_renderFrames=0,s_maxRenderUs=0;
static bool s_networkAudioHeld=false;
static void reserveRingingAudio(bool ringing) {
  if(ringing==s_networkAudioHeld) return;
  s_networkAudioHeld=ringing;
  if(ringing) services::background_network::suspendForAlertAudio();
  else services::background_network::resumeAfterAlertAudio();
}
static bool s_iconicPainted=false;
static time_t s_iconicMinute=0;

static bool iconicAlert() {
  return services::automation::isAlertActive() && !services::automation::activeAlertIsRecipe();
}
static bool timerAlert() { return services::automation::activeAlertServerId()==services::countdown::kServerId; }
static int wrapMessage(const char* message, char lines[][40], int maxLines, int charsPerLine) {
  if (!message || !message[0]) message = "Alert";
  int lineCount = 0;
  const char* cursor = message;

  while (*cursor && lineCount < maxLines) {
    while (*cursor == ' ') cursor++;
    if (!*cursor) break;

    int length = 0;
    int lastSpace = -1;
    while (cursor[length] && cursor[length] != '\n' && length < charsPerLine) {
      if (cursor[length] == ' ') lastSpace = length;
      length++;
    }

    int take = length;
    if (cursor[length] && cursor[length] != '\n' && length == charsPerLine && lastSpace > 0) {
      take = lastSpace;
    }
    if (take > 39) take = 39;
    memcpy(lines[lineCount], cursor, (size_t)take);
    lines[lineCount][take] = '\0';
    lineCount++;

    cursor += take;
    while (*cursor == ' ' || *cursor == '\n') cursor++;
  }

  return lineCount;
}

static void buildMorningScene(ui::scene::Scene& graph, int16_t width, int16_t height) {
  namespace style = morning_style;
  using Icon = style::Icon;
  const MorningPhase phase = services::automation::morningAlarmPhase();
  const bool sleeping = phase == MorningPhase::Sleeping;
  const bool greeting = phase == MorningPhase::Greeting;
  graph.clear();
  graph.addRect("morning.background", {0, 0, width, height}, style::kBg);
  char message[96];
  if(sleeping) {
    const uint32_t secondsLeft=(services::automation::morningAlarmSnoozeRemainingMs()+999u)/1000u;
    snprintf(message,sizeof(message),"Rest a little longer. I'll wake you again in %lu:%02lu.",
        (unsigned long)(secondsLeft/60),(unsigned long)(secondsLeft%60));
  } else snprintf(message,sizeof(message),"%s",greeting?"Good morning! Ready to start your day?":"Good morning! It's time to wake up.");
  kami_message::message(graph,width,height,"morning.message",message);
  if(sleeping)style::footer(graph,width,height,"B: Wake",Icon::Wake);
  else if(greeting)style::footer(graph,width,height,"B: Next",Icon::Next);
  else style::footer(graph,width,height,"B: Wake",Icon::Wake,"A: Snooze",Icon::Snooze);

}

static void buildAlertScene(ui::scene::Scene& sceneGraph, int16_t width, int16_t height) {
  if(iconicAlert()) {
    sceneGraph.clear();
    sceneGraph.addRect("alert.background",{0,0,width,height},timer_controls::kActive);
    if(timerAlert()) sceneGraph.addLabel("alert.timer.done",{24,202,(int16_t)(width-48),24},"Time's up!",kUiFg,timer_controls::kActive,2,scene::TextAlign::MiddleCenter,2);
    return;
  }
  if (services::automation::morningAlarmPhase() != MorningPhase::Idle) {
    buildMorningScene(sceneGraph, width, height);
    return;
  }
  if (services::automation::showingSnooze()) {
    sceneGraph.clear();
    ui::scene::Builder b(sceneGraph,width,height); b.clearContent(kUiBg);
    const uint32_t seconds=services::automation::snoozeRemainingSeconds();
    char message[96];
    snprintf(message,sizeof(message),"I've snoozed your alarm. I'll ring again in %02lu:%02lu.",
        (unsigned long)(seconds/60),(unsigned long)(seconds%60));
    kami_message::message(sceneGraph,width,height,"alert.snoozed",message);
    morning_style::footer(sceneGraph,width,height,"B: Dismiss",morning_style::Icon::Cancel,"A: Back",morning_style::Icon::Retry);
    return;
  }
  const services::automation::Type type = services::automation::activeAlertType();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  if (!services::automation::isAlertActive()) Serial.println("[morning-diag] stale generic alert frame");
#endif
  sceneGraph.clear();
  sceneGraph.addRect("alert.background",{0,0,width,height},services::automation::rgb565ForType(type));
  kami_message::message(sceneGraph,width,height,"alert.message",services::automation::activeAlertMsg());
  kami_message::footer3(sceneGraph,width,height,"A: Back","B: Dismiss","C: Snooze");

}

static void enterAlert() {
  s_iconicPainted=false;
  s_renderFrames=s_maxRenderUs=0;
  sfx::initSoundPlayer(s_ringSound);

  const services::automation::AlertSound sound = services::automation::activeAlertSound();
  s_wasRinging = services::automation::isAlertActive();
  reserveRingingAudio(s_wasRinging);
  if (s_wasRinging) {
    const auto& preset = sfx::alarms::preset((uint8_t)sound);
    s_ringSound.repeatFromDef(preset.ring, preset.repeatGapMs, millis());
  }
  s_lastSound = sound;
  s_lastPhase = services::automation::morningAlarmPhase();
  s_needRedraw = true;
  hal::display::markAllDirty();
}

static void exitAlert() {
  s_ringSound.stop();
  reserveRingingAudio(false);
}

static void tickAlert(uint32_t now) {
  const MorningPhase phase = services::automation::morningAlarmPhase();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  static uint8_t loggedPhase = 255, loggedExpression = 255;
  // A button handler may have started this phase after the loop's cached `now`.
  // Use a fresh time, as the renderer does, to avoid one underflowed log sample.
  const uint32_t elapsed = millis() - services::automation::morningAlarmPhaseStartedMs();
  const auto pose = morning_eyes::sample(phase, elapsed);
  if (phase != MorningPhase::Idle &&
      ((uint8_t)phase != loggedPhase || (uint8_t)pose.expression != loggedExpression)) {
    Serial.printf("[morning-eyes] phase=%u expression=%s elapsed_ms=%lu\n",
        (unsigned)phase, chibi_eyes::name(pose.expression), (unsigned long)elapsed);
  }
  loggedPhase = (uint8_t)phase;
  loggedExpression = (uint8_t)pose.expression;
#endif
  const bool ringing = services::automation::isAlertActive();
  reserveRingingAudio(ringing);
  const auto sound = services::automation::activeAlertSound();
  if (ringing != s_wasRinging || sound != s_lastSound) {
    s_ringSound.stop();
    if (ringing) {
      const auto& preset = sfx::alarms::preset((uint8_t)sound);
      s_ringSound.repeatFromDef(preset.ring, preset.repeatGapMs, now);
    }
  }
  if (phase == MorningPhase::Greeting && s_lastPhase != phase) {
    s_ringSound.playFromDef(sfx::kSfxBoot, now);
  }
  s_wasRinging = ringing;
  s_lastSound = sound;
  s_lastPhase = phase;
  // System alerts own their playback for the whole ringing lifecycle. A timer
  // can switch its underlying screen to Done while this overlay is active;
  // switchTo() clears the app audio group. Codec restoration also clears that
  // group. Neither may cancel an unacknowledged alarm's repetition policy.
  // Service once here, independently of the foreground app's audio group.
  s_ringSound.service(now);
  if (now - s_lastFrameMs >= 33u) {
    s_lastFrameMs = now;
    s_needRedraw = true;
  }
}

static void inputAlert(hal::input::InputEvent event) {
  using EventType = hal::input::EventType;
  const auto phase = services::automation::morningAlarmPhase();
  if (phase != MorningPhase::Idle) {
    if (event.type == EventType::ButtonC_Press && phase == MorningPhase::Ringing) {
      services::automation::snoozeActiveAlert();
    } else if (event.type == EventType::ButtonB_Press || event.type == EventType::ButtonA_Press) {
      services::automation::dismissActiveAlert();
    }
    // Stop the ring on the same input frame, before the audio task can repeat it.
    tickAlert(millis());
    s_needRedraw = true;
    return;
  }
  if(services::automation::showingSnooze()) {
    if(event.type==EventType::ButtonA_Tap) services::automation::leaveSnoozeScreen();
    else if(event.type==EventType::ButtonB_Tap) services::automation::dismissActiveAlert();
  } else if (event.type == EventType::ButtonC_Tap) {
    services::automation::snoozeActiveAlert();
  } else if (event.type == EventType::ButtonB_Tap || event.type == EventType::ButtonA_Tap) {
    services::automation::dismissActiveAlert();
  }
  tickAlert(millis());
  s_needRedraw=true;
}

static bool chordAlert() {
  return true;
}

static void drawAlert(TFT_eSprite& canvas) {
  if (!services::automation::wantsInputCapture()) return;
  if (!s_needRedraw) return;
  s_needRedraw = false;
  const uint32_t renderStart=micros();
  const bool iconic=iconicAlert();
  const time_t minute=time(nullptr)/60;
  const bool fullPaint=!iconic || !s_iconicPainted || minute!=s_iconicMinute;
  if(fullPaint) {
    ui::scene::Scene& sceneGraph = ui::scene::scratchScene();
    buildAlertScene(sceneGraph, canvas.width(), canvas.height());
    sceneGraph.render(canvas);
  }
  if(iconic) {
   if(fullPaint) {
    timer_controls::back(canvas);timer_controls::clock(canvas,timer_controls::kActive);
    canvas.drawLine(239,23,245,29,kUiFg);canvas.drawLine(245,29,257,17,kUiFg);
    canvas.drawLine(239,24,245,30,kUiFg);canvas.drawLine(246,29,258,17,kUiFg);
    timer_controls::snooze(canvas);
   }
    eye_rig::drawRingingEyes(canvas,millis(),timer_controls::kActive);
  }
  s_iconicPainted=iconic;s_iconicMinute=minute;
  ++s_renderFrames;s_maxRenderUs=max(s_maxRenderUs,(uint32_t)(micros()-renderStart));
  if(fullPaint) hal::display::markAllDirty();
  else hal::display::markDirty(90,100);
}

}  // namespace

#ifdef ORIGAMI_DEV_SERIAL_INPUT
void printAlertAudioStatus() {
  Serial.printf("[alarm-audio] ringing=%u repeat=%u playing=%u starts=%lu completed=%lu frames=%lu pos=%d ui_frames=%lu max_render_us=%lu\n",
      services::automation::isAlertActive(),s_ringSound.repeating,s_ringSound.playing,
      (unsigned long)s_ringSound.startedCount,(unsigned long)s_ringSound.completedCount,
      (unsigned long)s_ringSound.renderedFrames,s_ringSound.voice.pos,(unsigned long)s_renderFrames,(unsigned long)s_maxRenderUs);
}
#endif

const ::scene::Screen kScreenAutomationAlert = {
    "Automation Alert",
    enterAlert,
    exitAlert,
    tickAlert,
    inputAlert,
    chordAlert,
    drawAlert,
    "automation-alert",
    buildAlertScene,
    nullptr, nullptr, iconicAlert,
};

}  // namespace ui
