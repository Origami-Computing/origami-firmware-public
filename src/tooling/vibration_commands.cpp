#include "tooling/vibration_commands.h"
#ifdef ORIGAMI_DEV_SERIAL_INPUT
#include "core/alert_vibration.h"
#include "services/vibration.h"
#include "services/haptic_playback.h"
#include "hal/hal_imu.h"
#include <math.h>
#include "hal/hal_io.h"
#include "services.h"
#include <ArduinoJson.h>
#include <Arduino.h>
#include <string.h>

namespace tooling { namespace vibration_commands {
bool handleCommand(const char* line, bool alertBusy) {
  using namespace core::alert_vibration;
  using namespace services::vibration;
  if(strncmp(line,"vibe ",5))return false;
  JsonDocument req,res;
  res["ok"]=false;
  const auto err=deserializeJson(req,line+5);
  const char* op=req["op"] | "";
  res["op"]=op;
  if(err) res["error"]="Invalid JSON";
  else if(!strcmp(op,"hapticStatus")) {
    const auto state=services::haptic_playback::status();
    res["ok"]=true;res["version"]=1;res["driveRevision"]=2;res["slotUs"]=1000;
    res["highUs"]=state.highUs;res["elapsedUs"]=state.elapsedUs;
    res["errors"]=state.errors;res["maxGapUs"]=state.maxGapUs;
    res["playing"]=effectPlaying();res["enabled"]=navigationEnabled();
    res["maxGapMs"]=effectMaxGapMs();res["transitions"]=effectTransitions();
    res["starts"]=effectStarts();res["cue"]=unsigned(lastCue());
    res["busy"]=alertBusy;res["intensityMode"]="pulse-density";
  } else if(!strcmp(op,"hapticMeasure")) {
    // Bounded diagnostic: holds the UI on purpose to verify independent output.
    // Uses existing accelerometer configuration; no settings/NVS changes.
    if(alertBusy || !req["intensity"].is<unsigned>() || req["intensity"].as<unsigned>()>100 || !hal::imu::isReady())
      res["error"]="Invalid intensity, IMU unavailable, or motor busy";
    else {
      core::haptic::Effect e;e.count=1;e.pulses[0]={0,1200,req["intensity"].as<uint8_t>()};
      if(!playEffect(e))res["error"]="Motor busy";
      else {
        double sum[3]={},squares[3]={};unsigned samples=0;
        delay(200); // Exclude initial motor spin-up from steady-state comparison.
        for(unsigned i=0;i<160;++i) {
          int32_t a[3];
          if(hal::imu::readAcceleration(a)) {
            ++samples;
            for(unsigned axis=0;axis<3;++axis) {sum[axis]+=a[axis];squares[axis]+=double(a[axis])*a[axis];}
          }
          delay(5);
        }
        stopEffect();double variance=0;
        if(samples)for(unsigned axis=0;axis<3;++axis)variance+=squares[axis]/samples-pow(sum[axis]/samples,2);
        res["ok"]=samples==160;res["samples"]=samples;
        res["rmsMg"]=sqrt(variance>0?variance:0);res["intensity"]=e.pulses[0].intensity;
        const auto state=services::haptic_playback::status();
        res["highUs"]=state.highUs;res["elapsedUs"]=state.elapsedUs;
        res["maxGapUs"]=state.maxGapUs;res["errors"]=state.errors;
        services::notifyUserActivity();
      }
    }
  } else if(!strcmp(op,"hapticStop")) {stopEffect();res["ok"]=true;
  } else if(!strcmp(op,"hapticEnable") && req["enabled"].is<bool>()) {
    setNavigationEnabled(req["enabled"].as<bool>());res["ok"]=true;res["enabled"]=navigationEnabled();
  } else if(!strcmp(op,"hapticPlay") || !strcmp(op,"hapticTrace") || !strcmp(op,"hapticBind") || !strcmp(op,"hapticGet")) {
    core::haptic::Effect e;
    const char* target=req["target"] | "";
    const bool selection=!strcmp(target,"selection");
    const bool audition=!strcmp(op,"hapticPlay") || !strcmp(op,"hapticTrace");
    const bool get=!strcmp(op,"hapticGet");
    const bool back=!strcmp(target,"back"),dizzy=!strcmp(target,"dizzy");
    bool good=audition || selection || !strcmp(target,"scroll") || (get && (back||dizzy));
    if(get)e=savedEffect(back?Cue::Back:dizzy?Cue::Dizzy:selection?Cue::Select:Cue::Scroll);
    else {
      const auto rows=req["pulses"].as<JsonArray>();
      good=good && rows.size()>0 && rows.size()<=core::haptic::kMaxPulses;
      if(good) {
        e.count=rows.size();
        for(uint8_t i=0;i<e.count;++i) {
          const auto row=rows[i].as<JsonArray>();
          if(row.size()!=3 || !row[0].is<unsigned>() || !row[1].is<unsigned>() || !row[2].is<unsigned>() ||
             row[0].as<unsigned>()>10000 || row[1].as<unsigned>()>5000 || row[2].as<unsigned>()>100) {good=false;break;}
          e.pulses[i]={row[0].as<uint16_t>(),row[1].as<uint16_t>(),row[2].as<uint8_t>()};
        }
        good=good && core::haptic::valid(e);
      }
      if(good && !strcmp(op,"hapticTrace"))good=core::haptic::duration(e)<=1000 && hal::imu::isReady();
      if(good)good=audition ? (!alertBusy && playEffect(e)) : bindEffect(selection,e);
    }
    res["ok"]=good;
    if(good) {
      auto rows=res["pulses"].to<JsonArray>();
      for(uint8_t i=0;i<e.count;++i) {auto row=rows.add<JsonArray>();row.add(e.pulses[i].startMs);row.add(e.pulses[i].durationMs);row.add(e.pulses[i].intensity);}
      res["durationMs"]=core::haptic::duration(e);
      // Hash every ideal 1 ms drive sample for browser/firmware parity checks.
      uint32_t hash=2166136261u;
      for(uint32_t t=0;t<core::haptic::duration(e);++t)hash=(hash^uint32_t(core::haptic::driveAt(e,t)))*16777619u;
      res["driveHash"]=hash;res["driveRevision"]=2;services::notifyUserActivity();
      if(!strcmp(op,"hapticTrace")) {
        const uint32_t began=millis();auto samples=res["samples"].to<JsonArray>();
        auto edges=res["pinEdges"].to<JsonArray>();int lastPin=-1;unsigned readErrors=0;
        while(uint32_t(millis()-began)<core::haptic::duration(e)+300) {
          int32_t accel[3];const uint32_t t=millis()-began;
          if(hal::imu::readAcceleration(accel)) {
            auto row=samples.add<JsonArray>();row.add(t);
            for(unsigned axis=0;axis<3;++axis)row.add(accel[axis]);
          }
          uint8_t port=0,tx=0,rx=0;
          if(hal::io::readExpanderPort(0,&port,&tx,&rx)) {
            const int pin=(port>>IOX_VIBRATION_MOTOR)&1;
            if(pin!=lastPin) {auto edge=edges.add<JsonArray>();edge.add(millis()-began);edge.add(pin);lastPin=pin;}
          } else ++readErrors;
          delay(3);
        }
        res["pinReadErrors"]=readErrors;
        stopEffect();
        const auto state=services::haptic_playback::status();
        res["highUs"]=state.highUs;res["elapsedUs"]=state.elapsedUs;
        res["maxGapUs"]=state.maxGapUs;res["errors"]=state.errors;
      }
    } else res["error"]="Invalid effect or motor busy";
  } else if(!strcmp(op,"list")) {
    auto items=res["patterns"].to<JsonArray>();
    for(uint8_t i=0;i<kCount;++i) { auto o=items.add<JsonObject>(); o["id"]=i;o["name"]=name(i); }
    res["ok"]=true;
  } else if(!strcmp(op,"status")) {
    uint8_t p0,p1,p2;
    res["motor"]=hal::io::readExpanderInputs(&p0,&p1,&p2)?int((p0>>IOX_VIBRATION_MOTOR)&1):-1;
    res["preview"]=previewing(); res["busy"]=alertBusy; res["ok"]=true;
  } else if(!strcmp(op,"stop")) { stopPreview(); res["ok"]=true;
  } else if(!strcmp(op,"get") && req["id"].is<unsigned>() && req["id"].as<unsigned>()<kCount) {
    const auto& p=pattern(req["id"].as<uint8_t>());
    res["name"]=p.name;res["id"]=req["id"];res["cycleMs"]=cycleMs(p);res["leadMs"]=p.leadMs;
    auto pulses=res["pulses"].to<JsonArray>();
    for(uint8_t i=0;i<p.count;++i) { auto a=pulses.add<JsonArray>();a.add(p.pulses[i].onMs);a.add(p.pulses[i].offMs); }
    res["ok"]=true;
  } else if(!strcmp(op,"preview") || !strcmp(op,"save")) {
    Pattern p{}; p.version=1;
    bool good=false;
    if(!strcmp(op,"preview") && req["id"].is<unsigned>() && req["id"].as<unsigned>()<kCount) {
      if(!alertBusy)good=preview(req["id"].as<uint8_t>());
    } else {
      const char* label=req["name"] | "";
      auto pulses=req["pulses"].as<JsonArray>();
      const bool validLead=req["leadMs"].isNull() || (req["leadMs"].is<unsigned>() && req["leadMs"].as<unsigned>()<=10000);
      if(validLead && strlen(label)>0 && strlen(label)<=20 && pulses.size()>0 && pulses.size()<=kMaxPulses) {
        p.leadMs=req["leadMs"] | 0;
        strlcpy(p.name,label,sizeof(p.name));p.count=pulses.size();good=true;
        for(uint8_t i=0;i<p.count;++i) {
          auto pair=pulses[i].as<JsonArray>();
          if(pair.size()!=2 || !pair[0].is<unsigned>() || !pair[1].is<unsigned>() || pair[0].as<unsigned>()>5000 || pair[1].as<unsigned>()>10000) {good=false;break;}
          p.pulses[i]={pair[0].as<uint16_t>(),pair[1].as<uint16_t>()};
        }
        good=good && valid(p);
        if(good && !strcmp(op,"preview"))good=!alertBusy && previewCustom(p);
        else if(good)good=req["slot"].is<unsigned>() && req["slot"].as<unsigned>()<kCustomCount && saveCustom(req["slot"].as<uint8_t>(),p);
      }
    }
    res["ok"]=good;
    if(!good)res["error"]=alertBusy?"An alarm owns the motor":"Invalid pattern or save failed";
    if(good)services::notifyUserActivity();
  } else res["error"]="Unknown operation or invalid ID";
  Serial.print("[vibe] ");serializeJson(res,Serial);Serial.println();
  return true;
}
} }
#endif
