#pragma once

#include <stdint.h>

namespace core {
namespace time {

/**
 * Validity and deep-sleep drift record for the ESP32 RTC-timer wall clock, kept
 * in RTC_NOINIT memory by core::time (board_time.cpp). Pure data + policy so it
 * can be host-tested.
 *
 * The SoC keeps counting system time in the RTC timer through deep sleep,
 * light sleep and software/panic/watchdog resets; only a power-on (and any
 * reset that also clears the RTC domain) loses it. RTC_NOINIT memory has the
 * same lifetime but holds garbage after power-on, so the record is sealed with a
 * magic and checksum. It is (re)sealed when a trusted network sync lands and
 * around deep sleep.
 *
 * In deep sleep only the internal ~136 kHz RC slow clock counts time (this board
 * has no 32 kHz crystal: GPIO15/16 are the I2C bus). IDF converts the slept RC
 * ticks with the calibration measured at the *previous* boot, typically on a
 * warm chip; the RC runs faster as the sleeping chip cools (measured +0.13% to
 * +0.95% on rev-4). The wake path therefore rescales the interval by the
 * calibration measured at wake (`sleepCal` -> wake cal), then removes a small
 * learned residual rate (`sleepPpm`, refined by each SNTP sync after >=10 min of
 * deep sleep).
 */
struct RetainedClock {
  uint32_t magic;
  uint32_t syncCount;        // trusted syncs observed while this record has lived
  int64_t  lastSyncEpoch;    // wall-clock seconds at the most recent trusted sync
  int64_t  sleepEnterUs;     // wall-clock us when deep sleep began; 0 = not sleeping
  int64_t  sleepUsSinceSync; // RTC-measured deep-sleep us since the last sync
  int32_t  sleepPpm;         // residual deep-sleep rate error: + means the RC runs fast
  uint32_t sleepCal;         // RC period (Q13.19 us) IDF used when sleep began; 0 = unknown
  uint32_t check;
};

static constexpr uint32_t kRetainedClockMagic = 0x434c4b33u;  // "CLK3"
// Epoch below which the wall clock is treated as unset (~2023-11-14). The RTC
// timer restarts at 1970 after power-on, so a lost clock is always below this.
static constexpr int64_t kValidEpochThreshold = 1700000000;
// Small backwards tolerance between the recorded sync and the clock now
// (SNTP slews/steps, RTC recalibration and drift correction move it a little).
static constexpr int64_t kRetainedBackwardsSlackSeconds = 120;
// Learn the sleep rate only from enough sleep that SNTP's tens-of-ms
// uncertainty stays below ~100 ppm, and bound it to a physically plausible RC error.
static constexpr int64_t kMinLearnSleepUs = 600LL * 1000000LL;
static constexpr int32_t kMaxSleepPpm = 50000;

// Without a network sync the RC-timed clock can drift by minutes a day, so stop
// showing it after this long. Alarms and timestamps keep running on the clock.
static constexpr int64_t kClockDisplayMaxAgeSeconds = 5LL * 24 * 60 * 60;

/** True while a clock last synced at `lastSyncEpoch` is fresh enough to display. */
inline bool clockDisplayFresh(int64_t nowEpoch, int64_t lastSyncEpoch) {
  return lastSyncEpoch > 0 && nowEpoch - lastSyncEpoch <= kClockDisplayMaxAgeSeconds;
}

// Deep sleep can run up to ~1% fast or slow, so a far-off alarm is approached in
// stages: each sleep undershoots by 2% plus a minute, then re-measures. Inside
// this window the board stays up, so the alarm rings from the running clock.
static constexpr int64_t kScheduledWakeLeadSeconds = 90;

/** Seconds to sleep toward a target `remaining` seconds away; 0 means boot now. */
inline int64_t scheduledWakeStageSeconds(int64_t remaining) {
  if (remaining <= kScheduledWakeLeadSeconds) return 0;
  return remaining - remaining / 50 - 60;
}

inline uint32_t retainedClockChecksum(const RetainedClock& r) {
  uint32_t h = 2166136261u;
  auto mix = [&h](uint32_t w) {
    for (int i = 0; i < 4; ++i) {
      h ^= (w >> (8 * i)) & 0xffu;
      h *= 16777619u;
    }
  };
  auto mix64 = [&mix](int64_t v) {
    mix((uint32_t)(uint64_t)v);
    mix((uint32_t)((uint64_t)v >> 32));
  };
  mix(r.magic);
  mix(r.syncCount);
  mix64(r.lastSyncEpoch);
  mix64(r.sleepEnterUs);
  mix64(r.sleepUsSinceSync);
  mix((uint32_t)r.sleepPpm);
  mix(r.sleepCal);
  return h;
}

inline void sealRetainedClock(RetainedClock& r) {
  r.magic = kRetainedClockMagic;
  r.check = retainedClockChecksum(r);
}

inline bool retainedClockSealed(const RetainedClock& r) {
  return r.magic == kRetainedClockMagic && r.check == retainedClockChecksum(r) &&
         r.lastSyncEpoch > kValidEpochThreshold;
}

/** Record a trusted sync at `syncedEpoch`. Starts a fresh record (rate `freshPpm`) if `r` is garbage. */
inline void recordTrustedSync(RetainedClock& r, int64_t syncedEpoch, int32_t freshPpm) {
  if (retainedClockSealed(r)) {
    r.syncCount += 1u;
  } else {
    r.syncCount = 1u;
    r.sleepEnterUs = 0;
    r.sleepUsSinceSync = 0;
    r.sleepPpm = freshPpm;
    r.sleepCal = 0;
  }
  r.lastSyncEpoch = syncedEpoch;
  sealRetainedClock(r);
}

/**
 * True when the clock the RTC carried into this boot descends from a trusted
 * sync and can be shown without a new network sync. `powerOnReset` must be true
 * for a cold power-on (RTC timer restarted). `nowEpoch` is the clock as booted.
 */
inline bool retainedClockTrusted(const RetainedClock& r, bool powerOnReset, int64_t nowEpoch) {
  if (powerOnReset || !retainedClockSealed(r)) return false;
  if (nowEpoch <= kValidEpochThreshold) return false;  // RTC domain was reset
  return nowEpoch + kRetainedBackwardsSlackSeconds >= r.lastSyncEpoch;
}

/**
 * Rescale a slept interval that IDF converted with RC period `sleepCal` to the
 * period `wakeCal` measured at wake (a closer estimate of the cold sleeping RC).
 * Either value 0 (unknown) leaves the interval unchanged. Both are Q13.19 us.
 */
inline int64_t rescaleSleepUs(int64_t measuredUs, uint32_t sleepCal, uint32_t wakeCal) {
  if (measuredUs <= 0 || !sleepCal || !wakeCal) return measuredUs;
  // ticks = measured / sleepCal; interval = ticks * wakeCal
  return (int64_t)((long double)measuredUs * wakeCal / sleepCal);
}

/** Microseconds to subtract from an RTC-measured sleep of `measuredUs` at rate error `ppm`. */
inline int64_t sleepCorrectionUs(int64_t measuredUs, int32_t ppm) {
  if (measuredUs <= 0 || ppm <= -1000000) return 0;
  // measured = true * (1 + ppm/1e6)  =>  measured - true = measured * ppm / (1e6 + ppm)
  return (int64_t)((long double)measuredUs * ppm / (1000000.0L + ppm));
}

/**
 * Refine the sleep rate after a network sync applied `stepUs` (true - device;
 * negative when the corrected clock was still fast) to a clock that slept
 * `sleepUs` (RTC-measured) since the previous sync. Returns `currentPpm` when the
 * sleep is too short to measure.
 */
inline int32_t learnSleepPpm(int32_t currentPpm, int64_t stepUs, int64_t sleepUs) {
  if (sleepUs < kMinLearnSleepUs) return currentPpm;
  // The clock showed C = measured - correction; the sync says real time was C + step.
  const long double shown = (long double)(sleepUs - sleepCorrectionUs(sleepUs, currentPpm));
  const long double real = shown + (long double)stepUs;
  if (real <= 0) return currentPpm;
  long double next = ((long double)sleepUs / real - 1.0L) * 1000000.0L;
  if (next > kMaxSleepPpm) next = kMaxSleepPpm;
  if (next < -kMaxSleepPpm) next = -kMaxSleepPpm;
  return (int32_t)(next < 0 ? next - 0.5L : next + 0.5L);
}

}  // namespace time
}  // namespace core
