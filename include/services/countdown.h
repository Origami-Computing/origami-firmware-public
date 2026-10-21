#pragma once
#include <stdint.h>
#include "services/automation.h"
namespace services { namespace countdown {
// Single persisted device countdown; stable storage and automation identity.
// All calls stay on the UI task. This API does not own a screen or worker.
enum class TimerState : uint8_t { Idle = 0, Running = 1, Paused = 2, Done = 3 };
struct Record {
  TimerState state;
  uint32_t duration, startedAt, fireAt, remaining;
  services::automation::AlertSound sound;
  uint8_t vibration;
  uint32_t cycleDuration; // Current countdown span; original duration survives snooze.
};
constexpr uint32_t kServerId = 0x71A1E001u;
uint32_t clampDuration(uint32_t seconds);
Record load(); // Validates legacy stored values; no writes or queue changes.
// Legacy multi-key persistence: false can mean a partial write.
bool save(const Record& record);
bool schedule(const Record& record); // Replaces this countdown's pending entry.
void cancel(); // Only this countdown's pending entry, never an active alert.
enum class Result { Ok, InvalidArgument, InvalidState, ClockUnsynced, QueueFull, StorageError };
// Shared native/USB/Lua transitions. No navigation, sound playback or new task.
Result start(uint32_t seconds, services::automation::AlertSound sound, uint8_t vibration);
Result pause();
Result resume();
// Re-arm an expired countdown without changing its original reset duration.
Result snooze(uint32_t seconds);
Result reset();
Record snapshot(); // Observational: derive expiry/remaining; never writes or changes the queue.
uint32_t revision(); // UI-task mutation revision, scoped to this boot.
void poll(); // Restore missing scheduled entry or persist expiry; call at 1 Hz.
} }
