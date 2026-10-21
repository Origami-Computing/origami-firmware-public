#pragma once
#include "core/alert_vibration.h"
#include "core/haptic_effect.h"

namespace services { namespace vibration {
// UI-task motor and preview owner with independently scheduled haptic output. Last writer wins; this is not an exclusive lease.
// Raw diagnostic HAL writes bypass this bookkeeping. Release stops only its owner.
enum class Owner : uint8_t { None, Alarm, Pomodoro, Preview, Tennis, Bowling, Golf, Haptic, Navigation, Dizzy };
void drive(Owner owner,uint8_t id,uint32_t elapsedMs);
void release(Owner owner);
void stopAll();
bool preview(uint8_t id,uint32_t durationMs=5000);
bool previewCustom(const core::alert_vibration::Pattern& value,uint32_t durationMs=5000);
void stopPreview();
void servicePreview(uint32_t now,bool available);
bool previewing();
// UI-task one-shot feedback. Duration is 1..10000 ms. Last writer wins;
// release of a superseded owner never stops the newer owner's output.
bool pulse(Owner owner,uint32_t durationMs);
// Called by the UI loop even when a game's screen is covered. Alarm/Pomodoro
// admission cancels the pending game pulse without stopping a newer owner.
void servicePulse(uint32_t now,bool available);
// UI-task only. Copies input; one shot, bounded to 10 seconds. Alert/game/preview output
// takes priority; Studio may replace navigation, dizzy or Studio playback.
bool playEffect(const core::haptic::Effect& effect);
void serviceEffect(uint32_t now,bool available);
void stopEffect();
bool effectPlaying();
uint32_t effectMaxGapMs(); // observed worker scheduling gap, rounded up
uint32_t effectTransitions();
uint32_t effectStarts();
bool bindEffect(bool selection,const core::haptic::Effect& effect);
const core::haptic::Effect& boundEffect(bool selection);
void setNavigationEnabled(bool enabled);
bool navigationEnabled();
void navigationFeedback(bool selection);
enum class Cue : uint8_t { None, Scroll, Select, Back, Dizzy };
const core::haptic::Effect& savedEffect(Cue cue);
Cue lastCue(); // last successfully started built-in cue, for device verification
void navigationFeedback(Cue cue);
// UI dispatch starts the first semantic cue immediately, before lifecycle work.
// Nested cues are suppressed; fallback runs only if no cue was requested.
// Suppression must precede a cue (it cannot retract already played feedback).
void beginInputFeedback(bool back);
void endInputFeedback(Cue fallback);
void screenFeedback();
void suppressInputFeedback(); // consumed wake/no-op gestures
// Home view supplies its qualified shake gate; releases on exit/cover/sleep.
// Worker independently enforces the four-second maximum per admitted burst.
// Alerts, game feedback and Studio audition take priority.
void dizzyFeedback(bool active);
} }
