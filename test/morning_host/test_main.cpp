// Compile the real automation.cpp against minimal hardware stubs.
// See docs/morning_alarm.md for the host build command.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <Arduino.h>
#include "services/automation.h"
#include "services/vibration.h"
#include "services/haptic_playback.h"
#include "core/store.h"

static uint32_t nowMs = 1000;
static time_t nowEpoch = 1789066800;
static bool synced = true;
static bool vibrating = false;
TestSerial Serial;
uint32_t millis() { return nowMs; }
extern "C" time_t time(time_t* out) noexcept(noexcept(::time(out))) { if (out) *out = nowEpoch; return nowEpoch; }
namespace core { namespace time { bool isSynced() { return synced; } }}
namespace services { namespace automation { void transport_poll_service(uint32_t) {} void beginRecipeTransport() {} }}
namespace core { namespace store {
// Only the persistence boundary is fake; preset selection and motor ownership
// execute the real alert_vibration implementation. No custom slots are loaded.
bool getBlob(const char*, const char*, void*, size_t) { return false; }
bool putBlob(const char*, const char*, const void*, size_t) { return false; }
}}
namespace services { namespace morning_quote {
static time_t requestedEpoch = 0;
void requestForEpoch(time_t epochSeconds, bool) { requestedEpoch = epochSeconds; }
}}
namespace hal { namespace io { void setVibration(bool value) { vibrating = value; } }}
// These scheduler cases do not request navigation effects. Keep the independent
// motor worker behind its boundary; unexpected effect starts fail the fixture.
namespace services { namespace haptic_playback {
bool start(const core::haptic::Effect&) { assert(false); return false; }
bool startLoop(const core::haptic::Effect&,uint32_t) { assert(false); return false; }
void renewLoop() { assert(false); }
void stop(bool) {}
Status status() { return {}; }
}}
using namespace services::automation;
using Phase = core::morning_alarm::Phase;
static void tick(uint32_t advance = 0) {
  nowMs += advance;
  service(nowMs);
}
static void finish() {
  dismissActiveAlert();
  tick(core::morning_alarm::kGreetingMs);
  assert(!wantsInputCapture());
  assert(takeMorningRoutineRequest());
  assert(!takeMorningRoutineRequest());
  cancelAllPending();
}

static time_t localEpoch(int year, int month, int day, int hour, int minute) {
  tm value{};
  value.tm_year = year - 1900;
  value.tm_mon = month - 1;
  value.tm_mday = day;
  value.tm_hour = hour;
  value.tm_min = minute;
  value.tm_isdst = -1;
  return mktime(&value);
}
static void checkNext(int y, int m, int d, int h, int min, int nextY, int nextM, int nextD,
                      int alarmH, int alarmMin, int expectedHours = -1) {
  const time_t now = localEpoch(y, m, d, h, min);
  const time_t fire = core::morning_alarm::nextFire(now, alarmH, alarmMin);
  assert(fire == localEpoch(nextY, nextM, nextD, alarmH, alarmMin));
  assert(fire > now);
  if (expectedHours >= 0) assert(fire - now == expectedHours * 3600);
}

int main() {
  setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
  tzset();
  checkNext(2026, 9, 10, 6, 0, 2026, 9, 10, 7, 0, 1);
  checkNext(2026, 9, 10, 7, 0, 2026, 9, 11, 7, 0, 24);
  checkNext(2026, 3, 7, 7, 0, 2026, 3, 8, 7, 0, 23);
  checkNext(2026, 10, 31, 7, 0, 2026, 11, 1, 7, 0, 25);
  checkNext(2026, 12, 31, 23, 59, 2027, 1, 1, 0, 0);
  checkNext(2028, 2, 28, 7, 0, 2028, 2, 29, 7, 0, 24);
  assert(core::morning_alarm::nextFire(nowEpoch, 24, 0) == (time_t)-1);
  puts("PASS daily calendar, DST +/-1h, leap day, year rollover, invalid time");

  synced = false;
  assert(previewMorningAlarm(AlertSound::Gentle));
  assert(!previewMorningAlarm(AlertSound::Classic));
  tick();
  assert(vibrating && isAlertActive() && morningAlarmPhase() == Phase::Ringing);
  snoozeActiveAlert();
  assert(!vibrating && !isAlertActive() && wantsInputCapture());
  assert(morningAlarmSnoozeRemainingMs() == 300000);
  tick(299999);
  assert(morningAlarmPhase() == Phase::Sleeping);
  nowEpoch += 7200; // NTP correction must not affect snooze.
  tick(1);
  assert(morningAlarmPhase() == Phase::Ringing && isAlertActive());
  assert(activeAlertSound() == AlertSound::Gentle);
  snoozeActiveAlert();
  assert(morningAlarmSnoozeRemainingMs() == 300000);
  dismissActiveAlert();
  assert(morningAlarmPhase() == Phase::Greeting && !vibrating && !isAlertActive());
  tick(4499);
  assert(wantsInputCapture());
  assert(!takeMorningRoutineRequest());
  tick(1);
  assert(!wantsInputCapture());
  assert(takeMorningRoutineRequest());
  assert(!takeMorningRoutineRequest());
  tick(300001);
  assert(!isAlertActive());
  puts("PASS unsynced preview, exact five minutes, repeat snooze, NTP correction, early start day");

  nowMs = UINT32_MAX - 1000;
  assert(previewMorningAlarm(AlertSound::Chime));
  snoozeActiveAlert();
  tick(300000);
  assert(isAlertActive() && morningAlarmPhase() == Phase::Ringing);
  finish();
  puts("PASS millis rollover and normal acknowledgement");

  synced = true;
  assert(ingestScheduledWithSound(Type::Red, nowEpoch + 1, "Morning Alarm", 42,
                                AlertSound::Digital, AlertKind::Morning));
  tick();
  assert(!isAlertActive());
  nowEpoch++;
  tick();
  assert(isAlertActive() && morningAlarmPhase() == Phase::Ringing);
  assert(services::morning_quote::requestedEpoch == nowEpoch);
  assert(!hasPendingAutomationWithServerId(42));
  snoozeActiveAlert();
  assert(ingestScheduled(Type::Yellow, nowEpoch, "Ordinary alert", 43));
  assert(isAlertActive() && morningAlarmPhase() == Phase::Idle);
  assert(strcmp(activeAlertMsg(), "Ordinary alert") == 0);
  tick(300000);
  assert(strcmp(activeAlertMsg(), "Ordinary alert") == 0);
  dismissActiveAlert();
  tick();
  assert(isAlertActive() && morningAlarmPhase() == Phase::Ringing);
  assert(activeAlertSound() == AlertSound::Digital);
  finish();
  puts("PASS scheduled firing, ordinary alert during snooze, delayed re-ring restores sound");

  assert(previewMorningAlarm(AlertSound::Classic));
  for (unsigned i = 1; i <= 12; ++i) assert(ingestScheduled(Type::Yellow, nowEpoch + 100, "Queued", i));
  assert(!ingestScheduled(Type::Yellow, nowEpoch + 100, "Full", 13));
  snoozeActiveAlert();
  tick(300000);
  assert(isAlertActive() && morningAlarmPhase() == Phase::Ringing);
  assert(pendingAutomationsCount() == 12);
  finish();
  assert(ingestScheduled(Type::Green, nowEpoch, "Standard", 44));
  assert(morningAlarmPhase() == Phase::Idle);
  snoozeActiveAlert();
  assert(wantsInputCapture() && showingSnooze());
  assert(snoozeRemainingSeconds() == 60);
  leaveSnoozeScreen();
  assert(!wantsInputCapture() && pendingAutomationsCount() == 1);
  nowEpoch += 60;
  tick();
  assert(isAlertActive() && morningAlarmPhase() == Phase::Idle);
  dismissActiveAlert();
  puts("PASS full-queue morning snooze, unchanged standard one-minute snooze");

  const AlertSound regularAlarmSounds[] = {
      AlertSound::Classic,
      AlertSound::Chime,
      AlertSound::Digital,
      AlertSound::Gentle,
  };
  for (unsigned i = 0; i < sizeof(regularAlarmSounds) / sizeof(regularAlarmSounds[0]); ++i) {
    assert(ingestScheduledWithSound(Type::Red, nowEpoch, "Regular Alarm", 100 + i,
                                    regularAlarmSounds[i], AlertKind::Standard));
    assert(isAlertActive());
    assert(morningAlarmPhase() == Phase::Idle);
    assert(activeAlertSound() == regularAlarmSounds[i]);
    assert(strcmp(activeAlertMsg(), "Regular Alarm") == 0);
    dismissActiveAlert();
  }
  puts("PASS all four alarm sounds through the regular alarm path");
  // Admission and stable ordering belong to the scheduler, independently of
  // the saved-alarm catalog and network parsers.
  synced = false;
  assert(!schedule(Type::Yellow, nowEpoch, nullptr));
  assert(ingestScheduled(Type::Yellow, nowEpoch + 20, "later", 301));
  assert(ingestScheduled(Type::Green, nowEpoch + 10, "first", 302));
  assert(ingestScheduled(Type::Red, nowEpoch + 10, "second", 303));
  assert(ingestScheduled(Type::Red, nowEpoch - 1, "duplicate", 302));
  assert(pendingAutomationsCount() == 3);
  Type type; time_t fire; char message[128]; uint32_t id;
  assert(peekPendingAutomationDetail(0, &type, &fire, message, sizeof(message), &id));
  assert(id == 302 && type == Type::Green && strcmp(message, "first") == 0);
  assert(peekPendingAutomationDetail(1, &type, &fire, message, sizeof(message), &id));
  assert(id == 303 && strcmp(message, "second") == 0);
  assert(!peekPendingAutomation(-1, &type, &fire, message, sizeof(message)));
  assert(!peekPendingAutomation(3, &type, &fire, message, sizeof(message)));
  assert(!peekPendingAutomation(0, nullptr, &fire, message, sizeof(message)));
  assert(!peekPendingAutomation(0, &type, &fire, message, 0));
  assert(!cancelPendingAutomationAtIndex(3) && !cancelPendingWithServerId(0));
  assert(!shouldDeferBatterySleep() && !shouldExitBatterySleepLoop());
  tick(); assert(!isAlertActive());
  synced = true;
  assert(shouldDeferBatterySleep() && !shouldExitBatterySleepLoop());
  nowEpoch += 10;
  assert(shouldExitBatterySleepLoop());
  tick(); assert(strcmp(activeAlertMsg(), "first") == 0);
  // One active alert must not be overwritten by the other due event.
  tick(); assert(strcmp(activeAlertMsg(), "first") == 0);
  dismissActiveAlert(); tick();
  assert(strcmp(activeAlertMsg(), "second") == 0);
  dismissActiveAlert();
  assert(cancelPendingWithServerId(301) && pendingAutomationsCount() == 0);
  assert(!shouldDeferBatterySleep() && !shouldExitBatterySleepLoop());
  puts("PASS sorted/stable queue, dedupe, invalid reads, cancellation, sync and sleep gates");

  // Exercise the real shared vibration owner, with only the motor write fake.
  services::vibration::drive(services::vibration::Owner::Pomodoro, 0, 0);
  assert(vibrating); tick(); assert(vibrating);
  assert(ingestScheduled(Type::Yellow, nowEpoch, "Ownership", 304));
  tick(); assert(vibrating);
  tick(100); assert(!vibrating); // Classic preset's 20 ms off interval.
  tick(20); assert(vibrating);
  dismissActiveAlert(); assert(!vibrating);
  services::vibration::drive(services::vibration::Owner::Pomodoro, 0, 0);
  dismissActiveAlert(); tick(); assert(vibrating);
  services::vibration::release(services::vibration::Owner::Pomodoro);
  assert(!vibrating);
  puts("PASS idle scheduler preserves another motor owner; alarm pattern and release");
  puts("ALL MORNING ALARM HOST TESTS PASSED");
}
