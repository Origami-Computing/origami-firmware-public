#pragma once
#include <stdint.h>
namespace core {
enum class BatteryCue { None, Low, PluggedIn };
class BatteryAlertPolicy {
 public:
#if __cplusplus >= 201402L
  constexpr
#endif
  BatteryCue update(uint8_t percent, bool external, uint32_t now) {
    if (!initialized_) {
      initialized_=true; power_=candidatePower_=external; powerSince_=now;
    }
    if(external!=candidatePower_) { candidatePower_=external; powerSince_=now; }
    if(candidatePower_!=power_ && uint32_t(now-powerSince_)>=1000) {
      power_=candidatePower_; candidatePercent_=255; fired_=0;
      if(power_) return BatteryCue::PluggedIn;
    }
    // Suppress immediately on raw power presence; wait for a stable disconnect.
    if(external || power_ || percent>100) { candidatePercent_=255; return BatteryCue::None; }
    if(percent!=candidatePercent_) { candidatePercent_=percent; percentSince_=now; return BatteryCue::None; }
    if(uint32_t(now-percentSince_)<3000) return BatteryCue::None;
    const uint8_t levels[7]={10,5,4,3,2,1,0};
    uint8_t crossed=0;
    for(int i=0;i<7;++i) if(percent<=levels[i]) crossed|=uint8_t(1u<<i);
    const bool fresh=(crossed & ~fired_)!=0;
    fired_|=crossed;
    return fresh ? BatteryCue::Low : BatteryCue::None;
  }
 private:
  bool initialized_=false,power_=false,candidatePower_=false;
  uint8_t candidatePercent_=255,fired_=0;
  uint32_t powerSince_=0,percentSince_=0;
};
}
