#include "services/vibration.h"
#include "core/haptic_catalog.h"
#include "core/dizzy_feedback.h"
#include "hal/hal_io.h"
#include "services/haptic_playback.h"
#include <Arduino.h>

namespace services { namespace vibration {
using namespace core::alert_vibration;
namespace {
bool previewActive=false;
Pattern previewValue;
uint32_t previewStart=0,previewDuration=0;
Owner owner=Owner::None;
bool pulseActive=false;
uint32_t pulseStart=0,pulseDuration=0;
bool navEnabled=true,dizzyRequested=false;
core::haptic::Effect navTick,navSelect;
bool navInitialized=false;
uint32_t effectStartCount=0;
Cue recentCue=Cue::None;
bool inputFeedback=false,backInput=false,inputSuppressed=false,inputEmitted=false;
void initNavigation() {
  if(navInitialized)return;
  navInitialized=true;
  navTick=core::haptic::catalog::scroll;
  navSelect=core::haptic::catalog::select;
}
void output(Owner next,bool on) { haptic_playback::stop(next!=Owner::None); pulseActive=false; owner=next; hal::io::setVibration(on); }
}
void drive(Owner who,uint8_t id,uint32_t elapsed) { output(who,activeAt(pattern(id),elapsed)); }
void release(Owner who) { if(owner==who)output(Owner::None,false); }
void stopAll() { previewActive=false; output(Owner::None,false); }
bool previewCustom(const Pattern& p,uint32_t duration) {
  if(!valid(p) || duration<100 || duration>10000)return false;
  stopEffect();
  previewValue=p; previewStart=millis(); previewDuration=duration; previewActive=true; return true;
}
bool preview(uint8_t id,uint32_t duration) {
  if(id>=kCount)return false;
  if(id==kOff) { stopPreview(); return true; }
  return previewCustom(pattern(id),duration);
}
void stopPreview() { previewActive=false; release(Owner::Preview); }
void servicePreview(uint32_t now,bool available) {
  if(!previewActive)return;
  if(!available || uint32_t(now-previewStart)>=previewDuration) { stopPreview(); return; }
  output(Owner::Preview,activeAt(previewValue,now-previewStart));
}
bool previewing() { return previewActive; }
bool pulse(Owner who,uint32_t duration) {
  if(who==Owner::None || duration==0 || duration>10000)return false;
  output(who,true);
  pulseStart=millis();pulseDuration=duration;pulseActive=true;
  return true;
}
void servicePulse(uint32_t now,bool available) {
  if(pulseActive && (!available || uint32_t(now-pulseStart)>=pulseDuration))
    release(owner);
}
namespace {
bool startEffect(const core::haptic::Effect& e,Owner who) {
  if((owner==Owner::Haptic || owner==Owner::Navigation || owner==Owner::Dizzy) && !effectPlaying())owner=Owner::None;
  if(!core::haptic::valid(e) || previewActive ||
     (owner!=Owner::None && owner!=Owner::Navigation && owner!=Owner::Haptic && owner!=Owner::Dizzy))return false;
  if(who==Owner::Navigation && owner==Owner::Haptic)return false;
  if(who==Owner::Dizzy && owner!=Owner::None && owner!=Owner::Dizzy)return false;
  if(!(who==Owner::Dizzy ? haptic_playback::startLoop(e,core::haptic::DizzyFeedback::kMaximumMs) : haptic_playback::start(e)))return false;
  ++effectStartCount;
  pulseActive=false;owner=who;
  return true;
}
}
bool playEffect(const core::haptic::Effect& e) { if(!startEffect(e,Owner::Haptic))return false;recentCue=Cue::None;return true; }
void stopEffect() {
  if(owner==Owner::Haptic || owner==Owner::Navigation || owner==Owner::Dizzy)output(Owner::None,false);
}
void serviceEffect(uint32_t now,bool available) {
  (void)now;
  if(!available)stopEffect();
  else if(!effectPlaying() && (owner==Owner::Haptic || owner==Owner::Navigation || owner==Owner::Dizzy))owner=Owner::None;
}
bool effectPlaying(){return haptic_playback::status().playing;}
uint32_t effectMaxGapMs(){return (haptic_playback::status().maxGapUs+999)/1000;}
uint32_t effectTransitions(){return haptic_playback::status().transitions;}
uint32_t effectStarts(){return effectStartCount;}
bool bindEffect(bool selection,const core::haptic::Effect& e) {
  if(!core::haptic::valid(e))return false;
  initNavigation();(selection?navSelect:navTick)=e;return true;
}
const core::haptic::Effect& boundEffect(bool selection) {initNavigation();return selection?navSelect:navTick;}
void setNavigationEnabled(bool enabled) {navEnabled=enabled;if(!enabled && owner==Owner::Navigation)stopEffect();}
bool navigationEnabled(){return navEnabled;}
void navigationFeedback(bool selection) {
  navigationFeedback(selection?Cue::Select:Cue::Scroll);
}
const core::haptic::Effect& savedEffect(Cue cue) {
  if(cue==Cue::Scroll)return boundEffect(false);
  if(cue==Cue::Select)return boundEffect(true);
  if(cue==Cue::Dizzy)return core::haptic::catalog::dizzy;
  return core::haptic::catalog::back;
}
Cue lastCue(){return recentCue;}
void navigationFeedback(Cue cue) {
  if(cue==Cue::None || cue==Cue::Dizzy)return;
  if(inputFeedback){
    if(inputSuppressed || inputEmitted)return;
    // Admit once before lifecycle callbacks, never after their blocking work.
    inputEmitted=true;
  }
  if(navEnabled && startEffect(savedEffect(cue),Owner::Navigation))recentCue=cue;
}
void beginInputFeedback(bool back){inputFeedback=true;backInput=back;inputEmitted=false;inputSuppressed=false;}
void endInputFeedback(Cue fallback){
  const Cue cue=(inputSuppressed || inputEmitted)?Cue::None:fallback;
  inputFeedback=false;inputEmitted=false;navigationFeedback(cue);
}
void suppressInputFeedback(){if(inputFeedback)inputSuppressed=true;}
void screenFeedback(){navigationFeedback(inputFeedback && backInput?Cue::Back:Cue::Select);}
void dizzyFeedback(bool active){
  if(!active){dizzyRequested=false;release(Owner::Dizzy);return;}
  if(dizzyRequested){
    if(owner==Owner::Dizzy && effectPlaying())haptic_playback::renewLoop();
    return; // A lease expiry, stop or preemption cannot restart this gesture.
  }
  dizzyRequested=true;
  if(startEffect(savedEffect(Cue::Dizzy),Owner::Dizzy))recentCue=Cue::Dizzy;
}
} }
