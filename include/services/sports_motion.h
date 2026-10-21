#pragma once
/**
 * Motion input for the Wii-style games (bowling, golf): the gyro at 104 Hz through the IMU FIFO, so a ~12 fps
 * UI loop still sees every sample, plus a still-hold calibration of the gyro bias and "up".
 *
 *   open() on app entry, close() on exit (it holds a gyro lease and a PSRAM buffer only while open);
 *   each tick: n = poll(now) then samples()[0..n) oldest first, each with its own timestamp;
 *   calibration: calibrateBegin(); keep polling; calibrateStep(now) returns Done / Moved / Busy.
 * Pair with core::motion::SwingTracker for swing analysis.
 */
#include <stdint.h>

// UI-task-only shared sports session. There is one active consumer at a time.
// Output samples are borrowed until poll/close. Calibration retains its existing
// per-device bias estimate across sessions; no background task is created.
namespace services {
namespace motion_input {

struct Sample {
  uint32_t ms;       // when it was measured (the newest is ~now; 1/104 s apart)
  float dps[3];      // device-frame angular rate, degrees/second, NOT bias-corrected
};

bool open();                       // gyro lease + FIFO + buffer; false if the IMU is missing
void close();
bool isOpen();
int poll(uint32_t nowMs);          // drain the FIFO; returns how many samples()
const Sample* samples();

enum class Calibration : uint8_t { Busy, Done, Moved };
void calibrateBegin(uint32_t nowMs, uint16_t holdMs = 1000);
Calibration calibrateStep(uint32_t nowMs);   // call after poll(); Done once still for holdMs
const float* bias();               // gyro bias (dps): the live estimate (core/bias_estimator: resting or still
                                   // windows refine it, per device), seeded by the calibration
const float* upMg();               // accelerometer at the end of the hold (mg), valid after Done
bool readAccel(float outMg[3]);    // one accelerometer read (mg), e.g. to re-take "up" at address

}  // namespace motion_input
}  // namespace services
