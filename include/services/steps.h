#pragma once
#include <stdint.h>
namespace services { namespace steps {
struct Snapshot { uint32_t today, yesterday; uint16_t dailyGoal; };
// UI-task only. Poll loads once, enables the sensor when ready, samples every
// 1.5 seconds and persists dirty totals every 30 seconds. No dedicated task.
// Caller must honor the existing I2C/microphone scheduling restrictions.
void poll();
// Pure read, including before first poll (zero totals and the default goal).
Snapshot snapshot();
// Commands after initialization by poll(); reset re-baselines the hardware.
void resetToday();
void setDailyGoal(uint16_t goal); // Persist a changed goal, bounded to 1000..30000.
void flushDirty(); // Persist pending totals when leaving the view.
} }
