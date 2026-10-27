#pragma once
#include <stdint.h>

namespace services { namespace net_poll {
// Worker-owned, wrap-safe deadlines. No sleeps, transport or credential storage.
class RegistrationRetry {
 public:
  void onboarding(uint32_t now) {
    quick_ = 0; window_ = now; active_ = true;
    if (!restricted_) scheduled_ = false;
  }
  bool priority(uint32_t now) {
    if (active_ && uint32_t(now - window_) >= 45000) active_ = false;
    return active_ && !restricted_;
  }
  bool ready(uint32_t now) const {
    return !scheduled_ || uint32_t(now - since_) >= delay_;
  }
  void success() { active_ = false; scheduled_ = false; restricted_ = false; }
  uint32_t failed(uint32_t now, int code, uint32_t retryAfterMs = 0) {
    const bool transient = code < 0 || code == 408 || code == 502 || code == 503 || code == 504;
    restricted_ = code == 401 || code == 403 || code == 429 || retryAfterMs != 0;
    delay_ = 15000;
    if (!restricted_ && transient && priority(now) && quick_ < 2) {
      delay_ = (++quick_ == 1) ? 1000 : 2000;
    } else active_ = false;
    if (retryAfterMs > delay_) delay_ = retryAfterMs;
    since_ = now; scheduled_ = true;
    return delay_;
  }
 private:
  uint32_t since_ = 0, delay_ = 0, window_ = 0;
  unsigned quick_ = 0;
  bool scheduled_ = false, active_ = false, restricted_ = false;
};
} }
