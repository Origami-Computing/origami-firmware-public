#pragma once
#include "games/imu_roll_control.h"

namespace services { namespace doodle_controls {
// UI-task-only configuration shared by gameplay and developer controls.
// Owns validation and the saved schema; callers own redraw and presentation.
enum class LoadResult { Unchanged, Loaded, Invalid };
const games::imu_roll::Tuning& tuning();
// One attempt per boot, including missing/invalid records. Invalid retains tuning.
LoadResult load();
// Reset in memory only; neither clears the load latch nor writes storage.
void reset();
// Attempts every key; failure may leave a partially updated saved record.
bool save();
bool setMaxSpeed(float pxPerSec);
bool setDeadZones(float zeroDeadZoneDeg, float softDeadZoneDeg);
bool setSoftZoneFraction(float fraction);
bool setSign(float sign);
// Validate all four fields before changing any; sign is independent.
bool setTuning(float maxSpeedPxPerSec, float zeroDeadZoneDeg,
               float softDeadZoneDeg, float softZoneSpeedFraction);
} }
