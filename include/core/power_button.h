#pragma once
#include <stdint.h>

namespace core { namespace power_button {
constexpr uint32_t kHoldMs=1000;
enum class Action : uint8_t { None, Click, Sleep };
inline bool wakeRequested(bool /*a*/,bool /*b*/,bool c) { return c; }
class Gesture {
  bool down_=false, consumed_=false;
  uint32_t started_=0;
public:
  void suppress() { down_=false; consumed_=true; }
  Action update(bool down,uint32_t now) {
    if (!down) {
      const bool click=down_ && !consumed_;
      down_=false; consumed_=false;
      return click?Action::Click:Action::None;
    }
    if (consumed_) return Action::None;
    if (!down_) { down_=true; started_=now; }
    if ((uint32_t)(now-started_)>=kHoldMs) {
      consumed_=true; return Action::Sleep;
    }
    return Action::None;
  }
};
} }
