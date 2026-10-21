#pragma once
/**
 * Gyro bias (zero-rate offset) estimation for the motion games. Every LSM6DSL has its own offset (a few dps, per axis;
 * this kami: 1.36 / -2.58 / -0.26) and it moves with temperature, so it is measured on the device, continuously:
 * every 0.5 s window of samples is classified by its spread (max - min per axis):
 *   - resting (on a table, spread < 0.8 dps): the window's mean is the offset (blended strongly; the first one sets it);
 *   - held still by hand (spread < 8 dps) and close to the current estimate (< 4 dps): nudged gently (tremor averages
 *     out, a slow deliberate turn is too far off to count);
 *   - anything else (turning, swinging): ignored.
 * Wii Sports Resort does the same idea by asking you to put the remote down to calibrate MotionPlus.
 * Pure C++ (no Arduino); host test test/test_bias_estimator.
 */
#include <math.h>
#include <stdint.h>

namespace core {
namespace motion {

class BiasEstimator {
 public:
  static constexpr int kWindow = 52;               // 0.5 s at 104 Hz
  static constexpr float kRestSpread = 0.8f;       // dps: resting
  static constexpr float kHoldSpread = 8.0f;       // dps: held still by hand
  static constexpr float kRestMaxDelta = 6.0f;     // a resting window this far from the estimate is a smooth turn
  static constexpr float kHoldMaxDelta = 4.0f;
  static constexpr float kRestBlend = 0.8f, kHoldBlend = 0.2f;

  BiasEstimator() { reset(); }
  void reset() {
    valid_ = false;
    for (int k = 0; k < 3; ++k) bias_[k] = 0;
    restart();
  }
  // A starting estimate (e.g. a still-hold calibration); later windows correct it.
  void seed(const float b[3]) {
    for (int k = 0; k < 3; ++k) bias_[k] = b[k];
    valid_ = true;
  }
  // One raw gyro sample (dps, device frame, NOT bias-corrected). Returns true when the estimate changed.
  bool add(const float g[3]) {
    for (int k = 0; k < 3; ++k) {
      sum_[k] += g[k];
      if (g[k] < lo_[k]) lo_[k] = g[k];
      if (g[k] > hi_[k]) hi_[k] = g[k];
    }
    if (++n_ < kWindow) return false;
    float mean[3], spread = 0, delta = 0;
    for (int k = 0; k < 3; ++k) {
      mean[k] = sum_[k] / n_;
      spread = fmaxf(spread, hi_[k] - lo_[k]);
      delta = fmaxf(delta, fabsf(mean[k] - bias_[k]));
    }
    restart();
    float blend = 0;
    if (!valid_) blend = spread < kRestSpread ? 1.0f : 0.0f;
    else if (spread < kRestSpread && delta < kRestMaxDelta) blend = kRestBlend;
    else if (spread < kHoldSpread && delta < kHoldMaxDelta) blend = kHoldBlend;
    if (blend <= 0) return false;
    for (int k = 0; k < 3; ++k) bias_[k] += (mean[k] - bias_[k]) * blend;
    valid_ = true;
    return true;
  }
  bool valid() const { return valid_; }
  const float* bias() const { return bias_; }

 private:
  void restart() {
    n_ = 0;
    for (int k = 0; k < 3; ++k) { sum_[k] = 0; lo_[k] = 1e9f; hi_[k] = -1e9f; }
  }
  float bias_[3], sum_[3], lo_[3], hi_[3];
  int n_;
  bool valid_;
};

}  // namespace motion
}  // namespace core
