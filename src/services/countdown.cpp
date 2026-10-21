#include "services/countdown.h"
#include "core/store.h"
#include "core/alert_vibration.h"
#include <time.h>
namespace services { namespace countdown {
static constexpr char kPrefsNs[] = "timer";
static constexpr char kKeyState[] = "state";
static constexpr char kKeyDuration[] = "duration";
static constexpr char kKeyStarted[] = "started";
static constexpr char kKeyFire[] = "fire";
static constexpr char kKeyRemaining[] = "remain";
static constexpr char kKeySound[] = "sound";
static constexpr char kTimerMsg[] = "Timer";
static constexpr uint32_t kTimerServerId = kServerId;
static constexpr uint32_t kDefaultDurationSec = 5u * 60u;
static constexpr uint32_t kMaxDurationSec = 99u * 60u + 55u;
static constexpr uint32_t kMinDurationSec = 5u;

uint32_t clampDuration(uint32_t seconds) {
  if (seconds < kMinDurationSec) return kMinDurationSec;
  if (seconds > kMaxDurationSec) return kMaxDurationSec;
  return seconds;
}

static TimerState validState(uint8_t value) {
  if (value <= (uint8_t)TimerState::Done) return (TimerState)value;
  return TimerState::Idle;
}

static services::automation::AlertSound validSound(uint8_t value) {
  if (value <= (uint8_t)services::automation::AlertSound::Gentle) {
    return (services::automation::AlertSound)value;
  }
  return services::automation::AlertSound::Classic;
}

static uint32_t s_revision = 0;
static Record s_current{};
static bool s_loaded=false;
uint32_t revision() { return s_revision; }

bool save(const Record& record) {
  bool ok = true;
  ok &= core::store::putU8(kPrefsNs, kKeyState, (uint8_t)record.state);
  ok &= core::store::putU32(kPrefsNs, kKeyDuration, record.duration);
  ok &= core::store::putU32(kPrefsNs,"cycle",record.cycleDuration?record.cycleDuration:record.duration);
  ok &= core::store::putU32(kPrefsNs, kKeyStarted, record.startedAt);
  ok &= core::store::putU32(kPrefsNs, kKeyFire, record.fireAt);
  ok &= core::store::putU32(kPrefsNs, kKeyRemaining, record.remaining);
  ok &= core::store::putU8(kPrefsNs, kKeySound, (uint8_t)record.sound);
  ok &= core::store::putU8(kPrefsNs,"vibration",record.vibration);
  ++s_revision;
  s_loaded=false;
  if(ok){s_current=record;s_loaded=true;}
  return ok;
}

bool schedule(const Record& record) {
  services::automation::cancelPendingWithServerId(kTimerServerId);
  return services::automation::ingestScheduledWithSound(
      services::automation::Type::Red, (time_t)record.fireAt, kTimerMsg, kTimerServerId,
      record.sound,services::automation::AlertKind::Standard,record.vibration);
}

void cancel() { services::automation::cancelPendingWithServerId(kTimerServerId); }
Record load() {
  Record record{};
  record.vibration=core::alert_vibration::validId(core::store::getU8(kPrefsNs,"vibration",0));
  record.state = validState(core::store::getU8(kPrefsNs, kKeyState, (uint8_t)TimerState::Idle));
  record.duration = clampDuration(core::store::getU32(kPrefsNs, kKeyDuration, kDefaultDurationSec));
  record.cycleDuration=clampDuration(core::store::getU32(kPrefsNs,"cycle",record.duration));
  record.startedAt = core::store::getU32(kPrefsNs, kKeyStarted, 0);
  record.fireAt = core::store::getU32(kPrefsNs, kKeyFire, 0);
  record.remaining = clampDuration(core::store::getU32(kPrefsNs, kKeyRemaining, record.duration));
  record.sound = validSound(core::store::getU8(
      kPrefsNs, kKeySound, (uint8_t)services::automation::AlertSound::Classic));
  // Paused timers can legitimately have fewer than the five-second setup minimum.
  if (record.state == TimerState::Paused)
    record.remaining = core::store::getU32(kPrefsNs,kKeyRemaining,record.duration);
  if(record.remaining>kMaxDurationSec) record.remaining=kMaxDurationSec;
  if (record.state == TimerState::Done) record.remaining = 0;
  s_current=record;s_loaded=true;
  return record;
}
void poll() {
  Record record{};
  record.state = validState(core::store::getU8(kPrefsNs, kKeyState, (uint8_t)TimerState::Idle));
  record.duration = clampDuration(core::store::getU32(kPrefsNs, kKeyDuration, kDefaultDurationSec));
  record.cycleDuration=clampDuration(core::store::getU32(kPrefsNs,"cycle",record.duration));
  record.fireAt = core::store::getU32(kPrefsNs, kKeyFire, 0);
  record.sound = validSound(core::store::getU8(kPrefsNs, kKeySound, (uint8_t)services::automation::AlertSound::Classic));
  if (record.state != TimerState::Running) { cancel(); return; }
  if (!services::automation::wallClockSynced() || record.fireAt == 0) return;
  const uint32_t now = (uint32_t)time(nullptr);
  if (record.fireAt <= now) {
    // Preserve other keys and the already-fired alert, including its snooze.
    core::store::putU8(kPrefsNs, kKeyState, (uint8_t)TimerState::Done);
    core::store::putU32(kPrefsNs, kKeyRemaining, 0);
    ++s_revision;
    s_loaded=false;
    return;
  }
  if (!services::automation::hasPendingAutomationWithServerId(kTimerServerId)) {
    services::automation::ingestScheduledWithSound(
        services::automation::Type::Red, (time_t)record.fireAt, kTimerMsg, kTimerServerId,
        record.sound, services::automation::AlertKind::Standard,
        core::alert_vibration::validId(core::store::getU8(kPrefsNs,"vibration",0)));
  }
}

Record snapshot() {
  auto record = s_loaded ? s_current : load();
  if (record.state == TimerState::Running && services::automation::wallClockSynced()) {
    const uint32_t now = (uint32_t)time(nullptr);
    record.remaining = record.fireAt > now ? record.fireAt - now : 0;
    if (!record.remaining) record.state=TimerState::Done;
  }
  return record;
}
static Result commitScheduled(Record record) {
  if (!schedule(record)) return Result::QueueFull;
  record.state=TimerState::Running;
  // Do not claim atomic persistence: callers can inspect a partial stored result.
  return save(record) ? Result::Ok : Result::StorageError;
}
Result start(uint32_t seconds, services::automation::AlertSound sound, uint8_t vibration) {
  if(seconds!=clampDuration(seconds) || (uint8_t)sound>(uint8_t)services::automation::AlertSound::Gentle ||
     core::alert_vibration::validId(vibration)!=vibration) return Result::InvalidArgument;
  auto record=snapshot();
  if(record.state==TimerState::Running || record.state==TimerState::Paused) return Result::InvalidState;
  if(!services::automation::wallClockSynced()) return Result::ClockUnsynced;
  const uint32_t now=(uint32_t)time(nullptr);
  return commitScheduled({TimerState::Running,seconds,now,now+seconds,seconds,sound,vibration,seconds});
}
Result pause() {
  auto record=snapshot();
  if(record.state==TimerState::Paused || record.state==TimerState::Done) return Result::Ok;
  if(record.state!=TimerState::Running) return Result::InvalidState;
  cancel();
  record.state=record.remaining ? TimerState::Paused : TimerState::Done;
  return save(record) ? Result::Ok : Result::StorageError;
}
Result resume() {
  auto record=snapshot();
  if(record.state==TimerState::Running) return Result::Ok;
  if(record.state!=TimerState::Paused || !record.remaining) return Result::InvalidState;
  if(!services::automation::wallClockSynced()) return Result::ClockUnsynced;
  record.startedAt=(uint32_t)time(nullptr);record.fireAt=record.startedAt+record.remaining;
  return commitScheduled(record);
}
Result snooze(uint32_t seconds) {
  auto record=snapshot();
  if(record.state!=TimerState::Done) return Result::InvalidState;
  if(seconds!=clampDuration(seconds)) return Result::InvalidArgument;
  if(!services::automation::wallClockSynced()) return Result::ClockUnsynced;
  record.startedAt=(uint32_t)time(nullptr);record.fireAt=record.startedAt+seconds;record.remaining=seconds;record.cycleDuration=seconds;
  return commitScheduled(record);
}
Result reset() {
  auto record=load();cancel();record.state=TimerState::Idle;
  record.startedAt=record.fireAt=0;record.remaining=record.duration;record.cycleDuration=record.duration;
  return save(record) ? Result::Ok : Result::StorageError;
}
} }
