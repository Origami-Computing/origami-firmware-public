#include "services/battery_alerts.h"
#include "core/battery_alert_policy.h"
#include "hal/hal_power.h"
#include "sfx/sfx_lib.h"
#include "sfx/sfx_voice.h"
#include "sfx/audio_group.h"
#include <Arduino.h>
namespace services { namespace battery_alerts {
namespace {
core::BatteryAlertPolicy s_batteryPolicy;
uint32_t s_lowCount=0,s_plugCount=0,s_skippedCount=0,s_renderedFrames=0;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool s_preview=false;
uint8_t s_percent=255;
int s_power=0;
uint32_t s_previewStarted=0;
#endif
}
#ifdef ORIGAMI_DEV_SERIAL_INPUT
void setPreview(int percent,int power,bool audio) {
  const bool enabled=audio && percent>=0;
  if(enabled!=s_preview || percent<0) s_batteryPolicy=core::BatteryAlertPolicy{};
  s_preview=enabled; s_percent=uint8_t(percent); s_power=power; s_previewStarted=millis();
}
Diagnostics diagnostics() {
  return {s_lowCount,s_plugCount,s_skippedCount,s_renderedFrames,s_preview,sfx::volume()};
}
#endif
void service(uint32_t millis_now) {
  // Visual fixtures do not drive sound unless the explicit battery-demo mode is used.
  auto power=hal::power::read();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  if(s_preview && uint32_t(millis_now-s_previewStarted)>=120000) setPreview(-1,0,false);
  if(s_preview) { power.battery_percent=s_percent; power.charger=s_power==1 ? hal::power::ChargerState::Charging : s_power==2 ? hal::power::ChargerState::Done : hal::power::ChargerState::Off; }
#endif
  const auto cue=s_batteryPolicy.update(power.battery_percent,hal::power::externalPowerPresent(power),millis_now);
  if(cue==core::BatteryCue::None) return;
  bool busy=false;
  if(auto* group=sfx::activeAudioGroup()) for(unsigned i=0;i<group->count;++i)
    if(group->nodes[i] && (group->nodes[i]->playing || group->nodes[i]->repeating)) busy=true;
  if(sfx::volume()==0 || busy || !sfx::beginPlaybackSession()) {
    ++s_skippedCount;
    Serial.println("[battery-cue] skipped (muted or audio busy)");
    return;
  }
  const auto& def=cue==core::BatteryCue::Low ? sfx::kSfxLowBatteryTension : sfx::kSfxPluggedIn;
  sfx::SfxVoice voice; voice.start(def);
  while(voice.pump()) {}
  sfx::notePlaybackActivity(millis()+8000u);
  s_renderedFrames+=voice.pos;
  const bool complete=voice.pos==int(def.duration*22050.f);
  if(complete) { if(cue==core::BatteryCue::Low) ++s_lowCount; else ++s_plugCount; }
  Serial.printf("[battery-cue] kind=%s percent=%u complete=%d frames=%d\n",cue==core::BatteryCue::Low ? "low" : "plug",power.battery_percent,complete,voice.pos);
}
} }
