#pragma once
#include <stdint.h>
namespace core {
namespace cine {
// UI-task-owned modal wheel; all deadlines are wrap-safe millisecond ages.
class WheelControl {
  bool color_ = true, visible_ = false;
  uint32_t changedAt_ = 0;

public:
  void reset(bool color = true) {
    color_ = color;
    visible_ = false;
    changedAt_ = 0;
  }
  bool color() const { return color_; }
  bool visible() const { return visible_; }
  // Snap on for two seconds; the selected mode survives after dismissal.
  bool shown(uint32_t now) const { return visible_ && uint32_t(now - changedAt_) < 2000u; }
  void show(uint32_t now) {
    visible_ = true;
    changedAt_ = now;
  }
  void toggle(uint32_t now) {
    color_ = !color_;
    show(now);
  }
  bool expire(uint32_t now) {
    if (visible_ && uint32_t(now - changedAt_) >= 2000u) {
      visible_ = false;
      return true;
    }
    return false;
  }
};
} // namespace cine
} // namespace core
