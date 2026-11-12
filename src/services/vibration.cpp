#include "services/vibration.h"
#include "core/haptic_catalog.h"
#include "core/dizzy_feedback.h"
#include "hal/hal_io.h"
#include "services/haptic_playback.h"
#include "core/store.h"
#include <Arduino.h>
#include <string.h>

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
bool hapticsLoaded=false,hapticsOn=true;
bool feedbackOwner(Owner who){return who==Owner::Navigation||who==Owner::Dizzy||who==Owner::Widget||
  who==Owner::Tennis||who==Owner::Bowling||who==Owner::Golf;}
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
bool hapticsEnabled() {
  if(!hapticsLoaded){hapticsLoaded=true;bool saved=true;
    if(core::store::getBlob("vibration","haptics",&saved,sizeof(saved)))hapticsOn=saved;}
  return hapticsOn;
}
bool setHapticsEnabled(bool enabled) {
  if(!core::store::putBlob("vibration","haptics",&enabled,sizeof(enabled)))return false;
  hapticsLoaded=true;hapticsOn=enabled;
  if(!enabled && feedbackOwner(owner))output(Owner::None,false);
  if(!enabled)dizzyRequested=false;
  return true;
}
bool pulse(Owner who,uint32_t duration) {
  if(who==Owner::None || duration==0 || duration>10000)return false;
  if(feedbackOwner(who) && !hapticsEnabled())return false;
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
  if(feedbackOwner(who) && !hapticsEnabled())return false;
  if((owner==Owner::Haptic || owner==Owner::Navigation || owner==Owner::Dizzy || owner==Owner::Widget) && !effectPlaying())owner=Owner::None;
  if(!core::haptic::valid(e) || previewActive ||
     (owner!=Owner::None && owner!=Owner::Navigation && owner!=Owner::Haptic && owner!=Owner::Dizzy && owner!=Owner::Widget))return false;
  if(who==Owner::Widget && owner==Owner::Haptic)return false;
  if(who==Owner::Navigation && (owner==Owner::Haptic || owner==Owner::Widget))return false;
  if(who==Owner::Dizzy && owner!=Owner::None && owner!=Owner::Dizzy)return false;
  if(!(who==Owner::Dizzy ? haptic_playback::startLoop(e,core::haptic::DizzyFeedback::kMaximumMs) : haptic_playback::start(e)))return false;
  ++effectStartCount;
  pulseActive=false;owner=who;
  return true;
}
}
bool widgetCue(const char* name) {
  if(!name)return false;
  core::haptic::Effect e;
  bool inhale=!strcmp(name,"breathe_in"),exhale=!strcmp(name,"breathe_out");
  if(inhale||exhale){e.count=inhale?4:6;for(unsigned i=0;i<e.count;++i){auto& p=e.pulses[i];p.startMs=i*1000;p.durationMs=140;p.intensity=inhale?uint8_t(25+i*10):uint8_t(65-i*8);}}
  else if(!strcmp(name,"complete")){e.count=2;e.pulses[0]={0,120,45};e.pulses[1]={300,180,30};}
  else return false;
  return startEffect(e,Owner::Widget);
}
void stopWidgetCue(){release(Owner::Widget);}
bool playEffect(const core::haptic::Effect& e) { if(!startEffect(e,Owner::Haptic))return false;recentCue=Cue::None;return true; }
void stopEffect() {
  if(owner==Owner::Haptic || owner==Owner::Navigation || owner==Owner::Dizzy)output(Owner::None,false);
}
void serviceEffect(uint32_t now,bool available) {
  (void)now;
  if(!available){stopWidgetCue();stopEffect();}
  else if(!effectPlaying() && (owner==Owner::Haptic || owner==Owner::Navigation || owner==Owner::Dizzy || owner==Owner::Widget))owner=Owner::None;
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
