/** Daily pedometer state, hardware sampling and batched persistence.
 * UI-task only: share the existing serialized I2C access; no new worker.
 * The caller keeps the existing microphone/contention gate around poll().
 */
#include "services/steps.h"
#include <Arduino.h>
#include "core/store.h"
#include "services/automation.h"
#include "core/board_time.h"
#include "hal/hal_imu.h"
namespace services { namespace steps {
namespace {
constexpr char kPrefsNs[]  = "steps";
constexpr char kKeyToday[] = "today";
constexpr char kKeyYday[]  = "yday";
constexpr char kKeyDay[]   = "day";
constexpr char kKeyGoal[]  = "goal";

constexpr uint32_t kBackgroundDrainEveryMs = 1500;
constexpr uint32_t kPersistEveryMs         = 30000;
constexpr uint16_t kDailyGoalDefault       = 6000;

static uint32_t s_today      = 0;
static uint32_t s_yesterday  = 0;
static uint32_t s_dayKey     = 0;
static uint16_t s_dailyGoal  = kDailyGoalDefault;

static bool     s_baselined  = false;
static uint16_t s_lastHwRaw  = 0;

static uint32_t s_lastDrainMs   = 0;
static uint32_t s_lastPersistMs = 0;
static uint32_t s_dirtyTotal    = 0;

// ----- helpers ---------------------------------------------------------------

static uint32_t computeDayKey() {
  if (!services::automation::wallClockSynced()) return 0;
  return core::time::localDayKey();  // returns 0 if tz not yet configured
}

static void loadFromStore() {
  s_today     = core::store::getU32(kPrefsNs, kKeyToday, 0);
  s_yesterday = core::store::getU32(kPrefsNs, kKeyYday, 0);
  s_dayKey    = core::store::getU32(kPrefsNs, kKeyDay, 0);
  s_dailyGoal = core::store::getU16(kPrefsNs, kKeyGoal, kDailyGoalDefault);
}

static void persistNow() {
  core::store::putU32(kPrefsNs, kKeyToday, s_today);
  core::store::putU32(kPrefsNs, kKeyYday, s_yesterday);
  core::store::putU32(kPrefsNs, kKeyDay, s_dayKey);
  core::store::putU16(kPrefsNs, kKeyGoal, s_dailyGoal);
  s_dirtyTotal = 0; s_lastPersistMs = millis();
}

static void persistGoalNow() { core::store::putU16(kPrefsNs, kKeyGoal, s_dailyGoal); }

static void maybePersist(bool force) {
  if (!s_dirtyTotal && !force) return;
  const uint32_t now = millis();
  if (!force && (uint32_t)(now - s_lastPersistMs) < kPersistEveryMs) return;
  persistNow();
}

static void rolloverIfNewDay() {
  const uint32_t k = computeDayKey();
  if (k == 0) return;
  if (s_dayKey == 0) { s_dayKey = k; persistNow(); return; }
  if (k == s_dayKey) return;
  s_yesterday = s_today;
  s_today     = 0;
  s_dayKey    = k;
  persistNow();
}

static bool drainHwDelta() {
  if (!hal::imu::isStepCounterEnabled()) return false;
  uint16_t raw = 0;
  if (!hal::imu::getStepCount(&raw)) return false;
  if (!s_baselined) { s_lastHwRaw = raw; s_baselined = true; return false; }
  // Unsigned subtraction handles the 16-bit wrap transparently: if the
  // counter rolled from 65530 → 5 the delta is correctly computed as 11.
  uint16_t delta = (uint16_t)(raw - s_lastHwRaw);
  if (delta == 0) return false;
  s_lastHwRaw = raw;
  s_today += delta;
  s_dirtyTotal += delta;
  return true;
}


} // namespace

void poll() {
  static bool s_loaded = false;
  if (!s_loaded) { loadFromStore(); s_loaded = true; }
  if (!hal::imu::isStepCounterEnabled()) {
    if (hal::imu::isReady()) (void)hal::imu::enableStepCounter();
  }
  const uint32_t now = millis();
  if ((uint32_t)(now - s_lastDrainMs) >= kBackgroundDrainEveryMs) {
    s_lastDrainMs = now;
    rolloverIfNewDay();
    drainHwDelta();
    maybePersist(false);
  }
}

Snapshot snapshot() { return {s_today, s_yesterday, s_dailyGoal}; }
void flushDirty() { if (s_dirtyTotal) persistNow(); }
void resetToday() {
    s_today      = 0;
    s_dirtyTotal = 1;
    uint16_t raw = 0;
    if (hal::imu::getStepCount(&raw)) { s_lastHwRaw = raw; s_baselined = true; }
    persistNow();
}
void setDailyGoal(uint16_t goal) {
  if (goal < 1000) goal = 1000;
  if (goal > 30000) goal = 30000;
  if (goal == s_dailyGoal) return;
  s_dailyGoal = goal;
  persistGoalNow();
}
} }
