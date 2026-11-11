#pragma once

namespace services { namespace screen_brightness {
// Boot before UI startup; thereafter UI-task only. No task or heap allocation.
// The nonzero minimum keeps the Settings controls visible for recovery.
constexpr int kMinimum = 5;
constexpr int kDefault = 100;
void initialize();
int percent();
int savedPercent();
void set(int percent);  // live preview, clamped to [5,100]; does not write NVS
bool save();           // failure leaves the live value available for retry
void discard();        // restore the last successfully loaded/saved value
} }
