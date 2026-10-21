#include "services/alarms.h"
#include <Arduino.h>
#include <stdio.h>
#include "core/store.h"
#include "core/board_time.h"
#include "core/alert_vibration.h"
#include "services/morning_quote.h"
namespace services { namespace alarms {
static constexpr uint32_t kLocalAlarmServerIdBase = 0xA10A0001u;
static constexpr char     kLocalAlarmMsg[] = "Alarm";

static constexpr char kPrefsNs[] = "alarmclk";
static constexpr char kKeyCount[] = "count";
static constexpr char kKeyMigrated[] = "v2";
static constexpr char kLegacyKeyEn[] = "en";
static constexpr char kLegacyKeyH[] = "h";
static constexpr char kLegacyKeyM[] = "m";

static Snapshot s_snapshot;
static Alarm (&s_alarms)[kMaxAlarms] = s_snapshot.alarms;
static uint8_t& s_alarmCount = s_snapshot.count;
static Alarm& s_morningAlarm = s_snapshot.morning;
static bool& s_morningScheduled = s_snapshot.morningScheduled;
static constexpr uint32_t kMorningServerId = 0xA10B0001u;
static void makeSlotKey(char prefix, uint8_t index, char* out, size_t cap) {
  snprintf(out, cap, "%c%u", prefix, (unsigned)index);
}

static uint32_t serverIdForIndex(uint8_t index) {
  return kLocalAlarmServerIdBase + index;
}

static void saveAlarmSlot(uint8_t index) {
  if (index >= s_alarmCount) return;
  char key[8];
  makeSlotKey('h', index, key, sizeof(key));
  core::store::putU8(kPrefsNs, key, s_alarms[index].hour);
  makeSlotKey('m', index, key, sizeof(key));
  core::store::putU8(kPrefsNs, key, s_alarms[index].minute);
  makeSlotKey('e', index, key, sizeof(key));
  core::store::putBool(kPrefsNs, key, s_alarms[index].enabled);
  makeSlotKey('s', index, key, sizeof(key));
  core::store::putU8(kPrefsNs, key, (uint8_t)s_alarms[index].sound);
  makeSlotKey('v', index, key, sizeof(key));
  core::store::putU8(kPrefsNs, key, s_alarms[index].vibration);
}

static void saveAlarmCount() {
  core::store::putU8(kPrefsNs, kKeyCount, s_alarmCount);
}

Alarm loadMorning() {
  Alarm alarm;
  const uint32_t packed = core::store::getU32("morning", "settings", 7u);
  alarm.hour = packed & 0xFF;
  alarm.minute = (packed >> 8) & 0xFF;
  alarm.sound = (services::automation::AlertSound)((packed >> 16) & 3);
  alarm.vibration = core::alert_vibration::validId((packed >> 18) & 15);
  alarm.enabled = (packed & (1u << 24)) != 0;
  if (alarm.hour > 23) alarm.hour = 7;
  if (alarm.minute > 59) alarm.minute = 0;
  return alarm;
}

void reload() {
  s_morningAlarm = loadMorning();
  if (!core::store::getBool(kPrefsNs, kKeyMigrated, false)) {
    if (core::store::getBool(kPrefsNs, kLegacyKeyEn, false)) {
      s_alarmCount = 1;
      s_alarms[0].hour = core::store::getU8(kPrefsNs, kLegacyKeyH, 7);
      s_alarms[0].minute = core::store::getU8(kPrefsNs, kLegacyKeyM, 0);
      s_alarms[0].enabled = true;
      saveAlarmCount();
      saveAlarmSlot(0);
    }
    core::store::putBool(kPrefsNs, kKeyMigrated, true);
  }

  s_alarmCount = core::store::getU8(kPrefsNs, kKeyCount, 0);
  if (s_alarmCount > kMaxAlarms) s_alarmCount = kMaxAlarms;

  for (uint8_t i = 0; i < s_alarmCount; i++) {
    char key[8];
    makeSlotKey('h', i, key, sizeof(key));
    s_alarms[i].hour = core::store::getU8(kPrefsNs, key, 7);
    makeSlotKey('m', i, key, sizeof(key));
    s_alarms[i].minute = core::store::getU8(kPrefsNs, key, 0);
    makeSlotKey('e', i, key, sizeof(key));
    s_alarms[i].enabled = core::store::getBool(kPrefsNs, key, false);
    makeSlotKey('s', i, key, sizeof(key));
    const uint8_t sound = core::store::getU8(
        kPrefsNs, key, (uint8_t)services::automation::AlertSound::Classic);
    s_alarms[i].sound = sound <= (uint8_t)services::automation::AlertSound::Gentle
                            ? (services::automation::AlertSound)sound
                            : services::automation::AlertSound::Classic;
    if (s_alarms[i].hour > 23) s_alarms[i].hour = 7;
    makeSlotKey('v', i, key, sizeof(key));
    s_alarms[i].vibration = core::alert_vibration::validId(core::store::getU8(kPrefsNs,key,0));
    if (s_alarms[i].minute > 59) s_alarms[i].minute = 0;
  }
}

static time_t nextFireLocalHM(uint8_t hour, uint8_t minute) {
  if (!services::automation::wallClockSynced() || !core::time::timezoneConfigured()) {
    return (time_t)-1;
  }

  return core::morning_alarm::nextFire(time(nullptr), hour, minute);
}

static bool scheduleMorning() {
  services::automation::cancelPendingWithServerId(kMorningServerId);
  if (!s_morningAlarm.enabled) return true;
  const time_t fire = nextFireLocalHM(s_morningAlarm.hour, s_morningAlarm.minute);
  if (fire == (time_t)-1) return false;
  if (services::automation::morningAlarmPhase() == core::morning_alarm::Phase::Idle) {
    services::morning_quote::requestForEpoch(fire);
  }
  return services::automation::ingestScheduledWithSound(
      services::automation::Type::Red, fire, "Morning Alarm", kMorningServerId,
      s_morningAlarm.sound, services::automation::AlertKind::Morning, s_morningAlarm.vibration);
}

static bool scheduleAlarm(uint8_t index) {
  if (index >= s_alarmCount) return false;
  const uint32_t serverId = serverIdForIndex(index);
  services::automation::cancelPendingWithServerId(serverId);
  if (!s_alarms[index].enabled) return true;

  const time_t fire = nextFireLocalHM(s_alarms[index].hour, s_alarms[index].minute);
  if (fire == (time_t)-1) return false;
  return services::automation::ingestScheduledWithSound(
      services::automation::Type::Red, fire, kLocalAlarmMsg, serverId,
      s_alarms[index].sound, services::automation::AlertKind::Standard, s_alarms[index].vibration);
}

void prepareNextMorningQuote() {
  reload();
  if (!s_morningAlarm.enabled) return;
  const time_t fire = nextFireLocalHM(s_morningAlarm.hour, s_morningAlarm.minute);
  if (fire != (time_t)-1) services::morning_quote::requestForEpoch(fire);
}

void poll() {
  static uint32_t lastServiceMs = 0;
  const uint32_t now = millis();
  if ((uint32_t)(now - lastServiceMs) < 4000u) return;
  lastServiceMs = now;

  reload();
  if (!s_morningAlarm.enabled) {
    services::automation::cancelPendingWithServerId(kMorningServerId);
    s_morningScheduled = false;
  } else if (!services::automation::hasPendingAutomationWithServerId(kMorningServerId)) {
    s_morningScheduled = scheduleMorning();
  } else {
    s_morningScheduled = true;
  }
  for (uint8_t i = 0; i < kMaxAlarms; i++) {
    const uint32_t serverId = serverIdForIndex(i);
    if (i >= s_alarmCount || !s_alarms[i].enabled) {
      services::automation::cancelPendingWithServerId(serverId);
      continue;
    }
    if (!services::automation::wallClockSynced() || !core::time::timezoneConfigured()) continue;
    if (services::automation::hasPendingAutomationWithServerId(serverId)) continue;
    scheduleAlarm(i);
  }
}


const Snapshot& snapshot() { return s_snapshot; }
bool save(uint8_t index, const Alarm& alarm) {
  if (index > s_alarmCount || index >= kMaxAlarms) return false;
  if (index == s_alarmCount) { ++s_alarmCount; saveAlarmCount(); }
  s_alarms[index] = alarm;
  saveAlarmSlot(index);
  scheduleAlarm(index);
  return true;
}
bool saveMorning(const Alarm& alarm) {
  const uint32_t packed = alarm.hour | ((uint32_t)alarm.minute << 8) |
      ((uint32_t)alarm.sound << 16) | ((uint32_t)alarm.vibration << 18) |
      ((uint32_t)alarm.enabled << 24);
  if (!core::store::putU32("morning", "settings", packed) ||
      core::store::getU32("morning", "settings", ~packed) != packed) return false;
  s_morningAlarm = alarm;
  s_morningScheduled = scheduleMorning();
  return true;
}
} }
