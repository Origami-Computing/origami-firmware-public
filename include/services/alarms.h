#pragma once
#include <stdint.h>
#include "services/automation.h"
namespace services { namespace alarms {
constexpr uint8_t kMaxAlarms = 8;
struct Alarm {
  uint8_t hour = 7;
  uint8_t minute = 0;
  bool enabled = true;
  services::automation::AlertSound sound = services::automation::AlertSound::Classic;
  uint8_t vibration = 0;
};

struct Snapshot {
  Alarm alarms[kMaxAlarms];
  uint8_t count = 0;
  Alarm morning;
  bool morningScheduled = false;
};
// UI-task only; no worker or UI callbacks. The snapshot has stable storage.
const Snapshot& snapshot();
// Fresh Morning record only; no catalog migration, snapshot update or scheduling.
Alarm loadMorning();
void reload(); // Load and validate settings; includes the existing legacy migration.
// index==count appends; otherwise updates a slot. Retains legacy slot-write
// behavior: true means a valid slot, not transactional storage verification.
bool save(uint8_t index, const Alarm& alarm);
bool saveMorning(const Alarm& alarm); // Read-back verified write, then reschedule.
void poll(); // Reload/reconcile missing queue entries, internally limited to 4 s.
void prepareNextMorningQuote();
} }
