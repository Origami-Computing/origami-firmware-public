#pragma once

#include <stdint.h>
#include <time.h>

namespace core { namespace morning_alarm {

enum class Phase : uint8_t { Idle, Ringing, Sleeping, Greeting };
static constexpr uint32_t kSnoozeMs = 5u * 60u * 1000u;
static constexpr uint32_t kGreetingMs = 4500u;

// Monotonic timing: clock corrections cannot shorten or extend a snooze.
struct Session {
  Phase phase = Phase::Idle;
  uint32_t startedMs = 0;
  void start(uint32_t now) { phase = Phase::Ringing; startedMs = now; }
  void snooze(uint32_t now) {
    if (phase == Phase::Ringing) { phase = Phase::Sleeping; startedMs = now; }
  }
  void acknowledge(uint32_t now) {
    if (phase == Phase::Ringing || phase == Phase::Sleeping) {
      phase = Phase::Greeting;
      startedMs = now;
    }
  }
  uint32_t remainingMs(uint32_t now) const {
    const uint32_t elapsed = now - startedMs;
    return phase == Phase::Sleeping && elapsed < kSnoozeMs ? kSnoozeMs - elapsed : 0;
  }
  void tick(uint32_t now, bool otherAlertActive) {
    if (phase == Phase::Sleeping && !remainingMs(now) && !otherAlertActive) start(now);
    if (phase == Phase::Greeting && now - startedMs >= kGreetingMs) phase = Phase::Idle;
  }
};

// Rebuild the next calendar day in local time, rather than adding 24 hours.
// tm_isdst=-1 lets libc apply the DST rule for the target date.
inline time_t nextFire(time_t now, uint8_t hour, uint8_t minute) {
  if (hour > 23 || minute > 59) return (time_t)-1;
  struct tm local;
  if (!localtime_r(&now, &local)) return (time_t)-1;
  const int day = local.tm_mday;
  local.tm_hour = hour;
  local.tm_min = minute;
  local.tm_sec = 0;
  local.tm_isdst = -1;
  time_t fire = mktime(&local);
  if (fire == (time_t)-1 || fire > now) return fire;
  // Reconstruct after mktime, which can normalize a nonexistent spring hour.
  localtime_r(&now, &local);
  local.tm_mday = day + 1;
  local.tm_hour = hour;
  local.tm_min = minute;
  local.tm_sec = 0;
  local.tm_isdst = -1;
  return mktime(&local);
}

}}  // namespace core::morning_alarm
