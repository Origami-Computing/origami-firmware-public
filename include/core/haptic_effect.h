#pragma once
#include <stdint.h>

namespace core { namespace haptic {
// Profile schema 1: absolute millisecond offsets, non-overlapping pulses.
// Drive revision 2: 1 ms pulse-density slots, NOT calibrated acceleration.
// floor((n+1)*duty/100)-floor(n*duty/100) spreads energy through short pulses.
constexpr uint8_t kMaxPulses=12;
struct Pulse { uint16_t startMs, durationMs; uint8_t intensity; };
struct Effect { uint8_t count=0; Pulse pulses[kMaxPulses]{}; };
inline uint32_t duration(const Effect& e) {
  return e.count ? uint32_t(e.pulses[e.count-1].startMs)+e.pulses[e.count-1].durationMs : 0;
}
inline bool valid(const Effect& e) {
  if(!e.count || e.count>kMaxPulses)return false;
  uint32_t end=0;
  for(uint8_t i=0;i<e.count;++i) {
    const auto& p=e.pulses[i];
    if(p.startMs<end || p.durationMs<20 || p.durationMs>5000 || p.intensity>100)return false;
    end=uint32_t(p.startMs)+p.durationMs;
    if(end>10000)return false;
  }
  return true;
}
inline bool driveAt(const Effect& e,uint32_t elapsed) {
  for(uint8_t i=0;i<e.count;++i) {
    const auto& p=e.pulses[i];
    if(elapsed>=p.startMs && elapsed<uint32_t(p.startMs)+p.durationMs)
      return ((elapsed-p.startMs+1)*p.intensity)/100 !=
             ((elapsed-p.startMs)*p.intensity)/100;
  }
  return false;
}
} }
