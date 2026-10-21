#pragma once

#include <Arduino.h>
#include <math.h>

namespace games {
namespace imu_roll {

/**
 * Reusable tilt-to-horizontal-control helper for IMU-driven games.
 *
 * Flow for a game:
 *   1. Capture neutral with captureNeutralDeg(accel).
 *   2. Each frame, call mapAccelToControl(accel, neutralDeg, tuning).
 *   3. Use Control::velocityPxPerSec to move a player/object.
 *
 * The defaults match the Doodle Jump control prototype:
 *   - relative roll is clamped to [-40, 40] degrees
 *   - no movement below 3 degrees
 *   - tiny response from 3..5 degrees
 *   - squared response from 5..40 degrees
 */

struct Tuning {
  int axis;
  float sign;
  float zeroDeadZoneDeg;
  float softDeadZoneDeg;
  float maxRollDeg;
  float maxSpeedPxPerSec;
  float softZoneSpeedFraction;

  constexpr Tuning(int controlAxis = 1,
         float controlSign = 1.0f,
         float zeroDeadZone = 3.0f,
         float softDeadZone = 5.0f,
         float maxRoll = 40.0f,
         float maxSpeed = 280.0f,
         float softZoneFraction = 0.10f)
      : axis(controlAxis),
        sign(controlSign),
        zeroDeadZoneDeg(zeroDeadZone),
        softDeadZoneDeg(softDeadZone),
        maxRollDeg(maxRoll),
        maxSpeedPxPerSec(maxSpeed),
        softZoneSpeedFraction(softZoneFraction) {}
};

struct Control {
  float rawRollDeg = 0.0f;
  float neutralDeg = 0.0f;
  float relativeRollDeg = 0.0f;
  float clampedRollDeg = 0.0f;
  float velocityPxPerSec = 0.0f;
};

inline float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

inline float accelMagnitude(const int32_t accel[3]) {
  const float ax = (float)accel[0];
  const float ay = (float)accel[1];
  const float az = (float)accel[2];
  return sqrtf(ax * ax + ay * ay + az * az);
}

inline float rollDegFromAccel(const int32_t accel[3], const Tuning& tuning = Tuning()) {
  const float mag = accelMagnitude(accel);
  if (mag <= 1.0f || tuning.axis < 0 || tuning.axis > 2) return 0.0f;
  const float normalized = clampf((float)accel[tuning.axis] / mag, -1.0f, 1.0f);
  return asinf(normalized) * 57.2957795f * tuning.sign;
}

inline bool captureNeutralDeg(const int32_t accel[3], float* outNeutralDeg,
                              const Tuning& tuning = Tuning()) {
  if (!outNeutralDeg) return false;
  *outNeutralDeg = rollDegFromAccel(accel, tuning);
  return true;
}

inline float velocityFromRelativeRoll(float relativeRollDeg,
                                      const Tuning& tuning = Tuning()) {
  const float clamped = clampf(relativeRollDeg, -tuning.maxRollDeg, tuning.maxRollDeg);
  const float mag = fabsf(clamped);
  if (mag < tuning.zeroDeadZoneDeg) return 0.0f;

  const float dir = clamped < 0.0f ? -1.0f : 1.0f;
  float fraction = 0.0f;
  if (mag < tuning.softDeadZoneDeg) {
    const float u = (mag - tuning.zeroDeadZoneDeg) /
                    (tuning.softDeadZoneDeg - tuning.zeroDeadZoneDeg);
    fraction = tuning.softZoneSpeedFraction * u * u;
  } else {
    const float u = (mag - tuning.softDeadZoneDeg) /
                    (tuning.maxRollDeg - tuning.softDeadZoneDeg);
    fraction = tuning.softZoneSpeedFraction +
               (1.0f - tuning.softZoneSpeedFraction) * u * u;
  }

  return dir * tuning.maxSpeedPxPerSec * clampf(fraction, 0.0f, 1.0f);
}

inline Control mapAccelToControl(const int32_t accel[3], float neutralDeg,
                                 const Tuning& tuning = Tuning()) {
  Control control;
  control.rawRollDeg = rollDegFromAccel(accel, tuning);
  control.neutralDeg = neutralDeg;
  control.relativeRollDeg = control.rawRollDeg - neutralDeg;
  control.clampedRollDeg = clampf(control.relativeRollDeg,
                                  -tuning.maxRollDeg, tuning.maxRollDeg);
  control.velocityPxPerSec = velocityFromRelativeRoll(control.relativeRollDeg, tuning);
  return control;
}

}  // namespace imu_roll
}  // namespace games
