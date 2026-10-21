#pragma once
#include <stdint.h>

namespace core { namespace haptic {
// UI-owned gesture admission. One burst per sustained shake, then a quiet
// interval is required to rearm. All clocks are wrap-safe milliseconds.
struct DizzyFeedback {
  static constexpr uint32_t kQualifyMs=800, kMaximumMs=4000, kRearmMs=1000;
  bool qualifying=false, active=false, spent=false, calming=false;
  uint32_t qualifyAt=0, startedAt=0, calmAt=0;
  bool update(uint32_t now,bool strongShake,bool settling) {
    if(active && (settling || uint32_t(now-startedAt)>=kMaximumMs))active=false;
    if(spent) {
      if(settling) {
        if(!calming){calming=true;calmAt=now;}
        if(uint32_t(now-calmAt)>=kRearmMs){spent=false;calming=false;}
      } else calming=false;
      return active;
    }
    if(!strongShake || settling){qualifying=false;return false;}
    if(!qualifying){qualifying=true;qualifyAt=now;}
    if(uint32_t(now-qualifyAt)>=kQualifyMs) {
      active=spent=true;qualifying=false;startedAt=now;
    }
    return active;
  }
};
} }
