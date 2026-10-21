#pragma once
#include <stdint.h>

namespace services { namespace pomodoro {
// UI-task configuration record. App owns edits and when to persist them.
// No timer, worker, playback or hardware session is created here.
struct Settings {
  uint8_t focusMinutes=25, breakMinutes=5, sound=0, vibration=0;
  // Read all four keys each time; normalize saved durations and preset IDs.
  void load();
  // Attempt all keys in order, even after failure; no rollback or normalization.
  // Failure may leave a partial saved record. The in-memory draft is unchanged.
  bool save() const;
};
} }
