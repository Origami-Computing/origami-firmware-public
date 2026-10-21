/**
 * @file imu_calibration_app.cpp
 * @brief Standalone coordinate-frame calibration and live IMU visualizer.
 */

#ifdef MENU_APP

#include "apps/imu_calibration_app.h"
#include "ui/safe_area_draw.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "core/imu_calibration.h"
#include "hal/hal_display.h"
#include "services/motion.h"
#include "scene/screen.h"
#include "ui/scene.h"

namespace apps {
namespace imu_calibration_app {
namespace {

using core::imu_calibration::Orientation;
using core::imu_calibration::Profile;
using core::imu_calibration::Tracker;
using core::imu_calibration::Vec3;

static constexpr uint16_t kBg = 0x0862;
static constexpr uint16_t kPanel = 0x18E3;
static constexpr uint16_t kWhite = 0xFFFF;
static constexpr uint16_t kMuted = 0x9CF3;
static constexpr uint16_t kCyan = 0x2E7F;
static constexpr uint16_t kYellow = 0xFEC0;
static constexpr uint16_t kRed = 0xF986;
static constexpr uint16_t kGreen = 0x4F2C;
static constexpr uint16_t kGrid = 0x4A69;

static constexpr int kTrialsPerAxis = 3;
static constexpr int kNeutralSamples = 120;
static constexpr int kStabilityWindowSamples = 25;
static constexpr uint32_t kSampleIntervalMs = 8;
static constexpr uint32_t kButtonSettleMs = 750;
static constexpr uint32_t kArmStableMs = 300;
static constexpr uint32_t kMotionQuietMs = 600;
static constexpr uint32_t kMotionTimeoutMs = 8000;
static constexpr float kMotionStartDps = 10.0f;
static constexpr float kMotionStopDps = 5.0f;
static constexpr float kMinRotationDeg = 45.0f;
static constexpr float kMaxRotationDeg = 150.0f;
static constexpr float kMinPurity = 0.78f;

enum class Page : uint8_t {
  Home,
  Neutral,
  ReturnNeutral,
  Arming,
  Capture,
  TrialReview,
  Summary,
  Visualizer,
  Error,
};

struct TrialCapture {
  Vec3 rotation;
  float totalAngle;
  float accelMin;
  float accelMax;
  bool motionStarted;
  uint32_t startedMs;
  uint32_t quietSinceMs;
};

struct State {
  Page page;
  Profile profile;
  Tracker tracker;
  bool hasProfile;
  bool imuOk;
  bool neutralCollecting;
  bool neutralReady;
  bool trialAccepted;
  bool saveOk;
  int homeSelection;
  int axis;
  int trial;
  int neutralCount;
  int stabilityCount;
  Vec3 neutralAccelSum;
  Vec3 neutralGyroSum;
  Vec3 stabilityAccelSum;
  Vec3 stabilityGyroSum;
  float stabilityAccelMagnitudeSum;
  float stabilityAccelMagnitudeSquareSum;
  float stabilityGyroMagnitudeSquareSum;
  Vec3 neutralAccel;
  Vec3 gyroBias;
  Vec3 trialAxes[3][kTrialsPerAxis];
  float trialScores[3][kTrialsPerAxis];
  Vec3 measuredAxes[3];
  float axisQuality[3];
  TrialCapture capture;
  Vec3 lastAccel;
  Vec3 lastGyro;
  float lastGyroMagnitude;
  float lastAccelMagnitude;
  uint32_t lastSampleMs;
  uint32_t neutralStableSinceMs;
  uint32_t neutralSettleUntilMs;
  uint32_t armSettleUntilMs;
  uint32_t armStableSinceMs;
  float armGyroFiltered;
  float armAccelFiltered;
  float armAlignmentFiltered;
  char status[64];
};

static State s;
static services::motion::Gyroscope s_gyroscope;

const char* axisName(int axis) {
  if (axis == 0) return "ROLL";
  if (axis == 1) return "PITCH";
  return "YAW";
}

uint16_t axisColor(int axis) {
  // User-facing device convention: roll is longitudinal (green), pitch is
  // lateral (red), and yaw is vertical (cyan/blue).
  if (axis == 0) return kGreen;
  if (axis == 1) return kRed;
  return kCyan;
}

void setStatus(const char* text) {
  snprintf(s.status, sizeof(s.status), "%s", text ? text : "");
}

Vec3 correctedGyro() {
  return core::imu_calibration::add(
      s.lastGyro, core::imu_calibration::scale(s.gyroBias, -1.0f));
}

bool nearNeutral() {
  if (core::imu_calibration::length(s.neutralAccel) < 1.0f) return false;
  const Vec3 current = core::imu_calibration::normalize(s.lastAccel);
  const Vec3 neutral = core::imu_calibration::normalize(s.neutralAccel);
  const float alignment = core::imu_calibration::dot(current, neutral);
  const float gyroDps = core::imu_calibration::length(correctedGyro()) * 0.001f;
  return alignment > 0.966f && gyroDps < 8.0f &&
         s.lastAccelMagnitude > 850.0f && s.lastAccelMagnitude < 1150.0f;
}

Vec3 averageAxis(int axis) {
  Vec3 sum{0.0f, 0.0f, 0.0f};
  for (int trial = 0; trial < kTrialsPerAxis; ++trial) {
    sum = core::imu_calibration::add(
        sum, core::imu_calibration::scale(s.trialAxes[axis][trial],
                                          s.trialScores[axis][trial]));
  }
  return core::imu_calibration::normalize(sum);
}

float averageQuality(int axis) {
  float sum = 0.0f;
  for (int trial = 0; trial < kTrialsPerAxis; ++trial) sum += s.trialScores[axis][trial];
  return sum / (float)kTrialsPerAxis;
}

void beginNeutral() {
  s.page = Page::Neutral;
  s.neutralCollecting = false;
  s.neutralCount = 0;
  s.neutralStableSinceMs = 0;
  setStatus("Hold flat, face up, pointing forward");
  Serial.println("[imu-cal] stage=neutral waiting-for-button");
  hal::display::markAllDirty();
}

void beginAxisStage(int axis, int trial) {
  s.axis = axis;
  s.trial = trial;
  s.page = Page::ReturnNeutral;
  s.neutralStableSinceMs = 0;
  setStatus("Return to the neutral pose");
  Serial.printf("[imu-cal] axis=%s trial=%d stage=return-neutral\n",
                axisName(axis), trial + 1);
  hal::display::markAllDirty();
}

void startArming() {
  const uint32_t now = millis();
  s.page = Page::Arming;
  s.armSettleUntilMs = now + kButtonSettleMs;
  s.armStableSinceMs = 0;
  s.armGyroFiltered = s.lastGyroMagnitude;
  s.armAccelFiltered = s.lastAccelMagnitude;
  s.armAlignmentFiltered = core::imu_calibration::dot(
      core::imu_calibration::normalize(s.lastAccel),
      core::imu_calibration::normalize(s.neutralAccel));
  setStatus("Settling after button press");
  Serial.printf("[imu-cal] axis=%s trial=%d stage=arming settle=%lums\n",
                axisName(s.axis), s.trial + 1, (unsigned long)kButtonSettleMs);
  hal::display::markAllDirty();
}

void startCapture() {
  s.capture = TrialCapture{};
  s.capture.accelMin = 100000.0f;
  s.page = Page::Capture;
  setStatus("GO - rotate once, then hold still");
  Serial.printf("[imu-cal] axis=%s trial=%d stage=go waiting-for-motion\n",
                axisName(s.axis), s.trial + 1);
  hal::display::markAllDirty();
}

void finishTrial(bool accepted, const char* reason, Vec3 axis = {0.0f, 0.0f, 0.0f},
                 float score = 0.0f) {
  s.trialAccepted = accepted;
  if (accepted) {
    s.trialAxes[s.axis][s.trial] = axis;
    s.trialScores[s.axis][s.trial] = score;
  }
  setStatus(reason);
  s.page = Page::TrialReview;
  const float net = core::imu_calibration::length(s.capture.rotation);
  const float purity = s.capture.totalAngle > 0.1f ? net / s.capture.totalAngle : 0.0f;
  Serial.printf("[imu-cal] axis=%s trial=%d result=%s reason=\"%s\" "
                "net=%.1f total=%.1f purity=%.3f accel=%.0f..%.0f score=%.2f\n",
                axisName(s.axis), s.trial + 1, accepted ? "accepted" : "rejected",
                reason, net, s.capture.totalAngle, purity, s.capture.accelMin,
                s.capture.accelMax, score);
  hal::display::markAllDirty();
}

void evaluateCapture() {
  const float netAngle = core::imu_calibration::length(s.capture.rotation);
  const float purity = s.capture.totalAngle > 0.1f ? netAngle / s.capture.totalAngle : 0.0f;
  if (!s.capture.motionStarted || netAngle < kMinRotationDeg) {
    finishTrial(false, "Rejected: rotate at least 45 degrees");
    return;
  }
  if (netAngle > kMaxRotationDeg) {
    finishTrial(false, "Rejected: rotation was too large");
    return;
  }
  if (purity < kMinPurity) {
    finishTrial(false, "Rejected: rotation axis was mixed");
    return;
  }
  if (s.capture.accelMin < 650.0f || s.capture.accelMax > 1400.0f) {
    finishTrial(false, "Rejected: acceleration was shaky");
    return;
  }

  const Vec3 candidate = core::imu_calibration::normalize(s.capture.rotation);
  if (s.trial > 0 && core::imu_calibration::dot(candidate, s.trialAxes[s.axis][0]) < 0.86f) {
    finishTrial(false, "Rejected: direction did not repeat");
    return;
  }
  for (int previous = 0; previous < s.axis; ++previous) {
    if (fabsf(core::imu_calibration::dot(candidate, s.measuredAxes[previous])) > 0.65f) {
      finishTrial(false, "Rejected: too close to another axis");
      return;
    }
  }

  const float angleScore = 1.0f - fminf(fabsf(netAngle - 90.0f) / 90.0f, 0.5f);
  const float score = fminf(1.0f, purity * angleScore);
  char message[64];
  snprintf(message, sizeof(message), "Accepted: %.0f deg, quality %u%%", netAngle,
           (unsigned)(score * 100.0f));
  finishTrial(true, message, candidate, score);
}

void advanceAfterTrial() {
  if (!s.trialAccepted) {
    beginAxisStage(s.axis, s.trial);
    return;
  }
  if (++s.trial < kTrialsPerAxis) {
    beginAxisStage(s.axis, s.trial);
    return;
  }

  s.measuredAxes[s.axis] = averageAxis(s.axis);
  s.axisQuality[s.axis] = averageQuality(s.axis);
  if (++s.axis < 3) {
    beginAxisStage(s.axis, 0);
    return;
  }

  if (!core::imu_calibration::buildProfile(s.neutralAccel, s.gyroBias,
                                            s.measuredAxes, s.axisQuality, &s.profile)) {
    s.page = Page::Error;
    setStatus("Could not construct a valid coordinate frame");
    return;
  }
  s.page = Page::Summary;
  setStatus("Frame ready. Review before saving.");
  hal::display::markAllDirty();
}

void enterVisualizer() {
  if (!core::imu_calibration::isValid(s.profile)) return;
  s.tracker.reset(s.profile);
  s.page = Page::Visualizer;
  setStatus("B: recenter   A: home");
  hal::display::markAllDirty();
}

void resetStabilityWindow() {
  s.stabilityCount = 0;
  s.stabilityAccelSum = {0.0f, 0.0f, 0.0f};
  s.stabilityGyroSum = {0.0f, 0.0f, 0.0f};
  s.stabilityAccelMagnitudeSum = 0.0f;
  s.stabilityAccelMagnitudeSquareSum = 0.0f;
  s.stabilityGyroMagnitudeSquareSum = 0.0f;
}

void sampleNeutral(uint32_t now) {
  if (!s.neutralCollecting) return;
  if ((int32_t)(now - s.neutralSettleUntilMs) < 0) return;

  s.stabilityAccelSum = core::imu_calibration::add(s.stabilityAccelSum, s.lastAccel);
  s.stabilityGyroSum = core::imu_calibration::add(s.stabilityGyroSum, s.lastGyro);
  s.stabilityAccelMagnitudeSum += s.lastAccelMagnitude;
  s.stabilityAccelMagnitudeSquareSum += s.lastAccelMagnitude * s.lastAccelMagnitude;
  s.stabilityGyroMagnitudeSquareSum += s.lastGyroMagnitude * s.lastGyroMagnitude;
  if (++s.stabilityCount < kStabilityWindowSamples) return;

  const float inverse = 1.0f / (float)s.stabilityCount;
  const float accelMean = s.stabilityAccelMagnitudeSum * inverse;
  const float accelVariance = fmaxf(0.0f,
      s.stabilityAccelMagnitudeSquareSum * inverse - accelMean * accelMean);
  const float accelStdDev = sqrtf(accelVariance);
  const float gyroRms = sqrtf(s.stabilityGyroMagnitudeSquareSum * inverse);
  const bool stable = gyroRms < 8.0f && accelMean > 850.0f && accelMean < 1150.0f &&
                      accelStdDev < 35.0f;

  if (stable) {
    s.neutralAccelSum = core::imu_calibration::add(s.neutralAccelSum,
                                                    s.stabilityAccelSum);
    s.neutralGyroSum = core::imu_calibration::add(s.neutralGyroSum,
                                                   s.stabilityGyroSum);
    s.neutralCount += s.stabilityCount;
    setStatus("Stable samples collecting");
  } else {
    // A bump restarts only the consecutive stable window. The user remains in
    // capture and does not need to press the encoder again.
    s.neutralCount = 0;
    s.neutralAccelSum = {0.0f, 0.0f, 0.0f};
    s.neutralGyroSum = {0.0f, 0.0f, 0.0f};
    setStatus("Movement detected - settling again");
    Serial.printf("[imu-cal] neutral window=rejected gyro-rms=%.2f "
                  "accel-mean=%.1f accel-sd=%.1f\n",
                  gyroRms, accelMean, accelStdDev);
  }
  resetStabilityWindow();
  if (s.neutralCount < kNeutralSamples) return;

  s.neutralAccel = core::imu_calibration::scale(s.neutralAccelSum,
                                                1.0f / (float)s.neutralCount);
  s.gyroBias = core::imu_calibration::scale(s.neutralGyroSum,
                                            1.0f / (float)s.neutralCount);
  s.neutralCollecting = false;
  s.neutralReady = true;
  Serial.printf("[imu-cal] neutral result=accepted samples=%d accel=(%.1f,%.1f,%.1f) "
                "gyro-bias=(%.1f,%.1f,%.1f)\n",
                s.neutralCount, s.neutralAccel.x, s.neutralAccel.y, s.neutralAccel.z,
                s.gyroBias.x, s.gyroBias.y, s.gyroBias.z);
  beginAxisStage(0, 0);
}

void sampleCapture(uint32_t now, float dt) {
  Vec3 gyro = correctedGyro();
  const float magnitudeDps = core::imu_calibration::length(gyro) * 0.001f;
  if (s.lastAccelMagnitude < s.capture.accelMin) s.capture.accelMin = s.lastAccelMagnitude;
  if (s.lastAccelMagnitude > s.capture.accelMax) s.capture.accelMax = s.lastAccelMagnitude;

  if (!s.capture.motionStarted && magnitudeDps >= kMotionStartDps) {
    s.capture.motionStarted = true;
    s.capture.startedMs = now;
    Serial.printf("[imu-cal] axis=%s trial=%d motion=start gyro=%.1fdps\n",
                  axisName(s.axis), s.trial + 1, magnitudeDps);
  }
  if (s.capture.motionStarted) {
    s.capture.rotation = core::imu_calibration::add(
        s.capture.rotation, core::imu_calibration::scale(gyro, dt * 0.001f));
    s.capture.totalAngle += magnitudeDps * dt;
    if (magnitudeDps < kMotionStopDps) {
      if (s.capture.quietSinceMs == 0) s.capture.quietSinceMs = now;
      if ((uint32_t)(now - s.capture.quietSinceMs) >= kMotionQuietMs) evaluateCapture();
    } else {
      s.capture.quietSinceMs = 0;
    }
  }

  // Waiting to begin is intentionally unlimited. The timeout only protects an
  // attempt after deliberate motion has crossed the start threshold.
  if (s.page == Page::Capture && s.capture.motionStarted &&
      (uint32_t)(now - s.capture.startedMs) > kMotionTimeoutMs) {
    finishTrial(false, "Rejected: capture timed out");
  }
}

void tick(uint32_t now) {
  if (!s_gyroscope.active() || (uint32_t)(now - s.lastSampleMs) < kSampleIntervalMs) return;
  const float dt = s.lastSampleMs == 0 ? 0.008f : (float)(now - s.lastSampleMs) * 0.001f;
  s.lastSampleMs = now;

  int32_t accel[3] = {};
  int32_t gyro[3] = {};
  s.imuOk = services::motion::readAxes(accel, gyro);
  if (!s.imuOk) {
    s.page = Page::Error;
    setStatus("IMU read failed");
    return;
  }
  s.lastAccel = {(float)accel[0], (float)accel[1], (float)accel[2]};
  s.lastGyro = {(float)gyro[0], (float)gyro[1], (float)gyro[2]};
  s.lastAccelMagnitude = core::imu_calibration::length(s.lastAccel);
  s.lastGyroMagnitude = core::imu_calibration::length(s.lastGyro) * 0.001f;

  if (s.page == Page::Neutral) sampleNeutral(now);
  if (s.page == Page::ReturnNeutral) {
    if (nearNeutral()) {
      if (s.neutralStableSinceMs == 0) s.neutralStableSinceMs = now;
      s.neutralReady = (uint32_t)(now - s.neutralStableSinceMs) >= 500u;
    } else {
      s.neutralStableSinceMs = 0;
      s.neutralReady = false;
    }
  }
  if (s.page == Page::Arming) {
    const Vec3 currentAccel = core::imu_calibration::normalize(s.lastAccel);
    const Vec3 neutralAccel = core::imu_calibration::normalize(s.neutralAccel);
    const float alignment = core::imu_calibration::dot(currentAccel, neutralAccel);
    const float correctedDps = core::imu_calibration::length(correctedGyro()) * 0.001f;
    constexpr float kFilter = 0.16f;
    s.armGyroFiltered += (correctedDps - s.armGyroFiltered) * kFilter;
    s.armAccelFiltered += (s.lastAccelMagnitude - s.armAccelFiltered) * kFilter;
    s.armAlignmentFiltered += (alignment - s.armAlignmentFiltered) * kFilter;

    if ((int32_t)(now - s.armSettleUntilMs) >= 0) {
      const bool stable = s.armGyroFiltered < 8.0f &&
                          s.armAccelFiltered > 850.0f && s.armAccelFiltered < 1150.0f &&
                          s.armAlignmentFiltered > 0.966f;
      if (stable) {
        if (s.armStableSinceMs == 0) s.armStableSinceMs = now;
        if ((uint32_t)(now - s.armStableSinceMs) >= kArmStableMs) startCapture();
      } else {
        s.armStableSinceMs = 0;
        setStatus("Hold neutral while the device settles");
      }
    }
  }
  if (s.page == Page::Capture) sampleCapture(now, dt);
  if (s.page == Page::Visualizer) s.tracker.update(s.profile, s.lastAccel, s.lastGyro, dt);
  hal::display::markAllDirty();
}

void enter() {
  s = State{};
  s.hasProfile = core::imu_calibration::load(&s.profile);
  s_gyroscope.open();
  s.imuOk = s_gyroscope.active();
  s.page = s_gyroscope.active() ? Page::Home : Page::Error;
  s.homeSelection = 0;
  setStatus(s_gyroscope.active() ? "Create or inspect the default IMU frame"
                           : "Gyroscope could not be enabled");
  hal::display::markAllDirty();
}

void exitApp() {
  s_gyroscope.close();
}

struct Point {
  int16_t x;
  int16_t y;
};

Point project(Vec3 point, int cx, int cy, float scale) {
  // Tracker coordinates are semantic {roll, pitch, yaw}. Present them as a
  // device frame with pitch across the screen, roll receding in depth, and
  // yaw vertical. Keeping this mapping in the projector makes the sphere,
  // axis vectors, and device body obey the same geometry.
  return {(int16_t)(cx + scale * (point.y * 0.86f - point.x * 0.34f)),
          (int16_t)(cy - scale * (point.z * 0.82f + point.x * 0.28f))};
}

Orientation toVisualizerOrientation(Orientation orientation) {
  // The screen model uses the opposite pitch/yaw convention from the tracker.
  // This is the equivalent of expressing the quaternion in a frame rotated
  // 180 degrees about roll, preserving roll while reversing pitch and yaw.
  return {orientation.w, orientation.x, -orientation.y, -orientation.z};
}

void drawLoop(TFT_eSprite& canvas, int plane, int cx, int cy, float radius,
              uint16_t color) {
  Point previous{};
  for (int i = 0; i <= 36; ++i) {
    const float angle = (float)i * 6.2831853f / 36.0f;
    Vec3 point{};
    if (plane == 0) point = {cosf(angle), sinf(angle), 0.0f};
    if (plane == 1) point = {cosf(angle), 0.0f, sinf(angle)};
    if (plane == 2) point = {0.0f, cosf(angle), sinf(angle)};
    const Point current = project(point, cx, cy, radius);
    if (i > 0) canvas.drawLine(previous.x, previous.y, current.x, current.y, color);
    previous = current;
  }
}

void drawVisualizer(TFT_eSprite& canvas) {
  const int cx = canvas.width() / 2;
  const int cy = 108;
  const float radius = 76.0f;
  drawLoop(canvas, 0, cx, cy, radius, kGrid);
  drawLoop(canvas, 1, cx, cy, radius, kGrid);
  drawLoop(canvas, 2, cx, cy, radius, kGrid);

  const Orientation orientation = toVisualizerOrientation(s.tracker.orientation());
  const Vec3 localAxes[3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                             {0.0f, 0.0f, 1.0f}};
  const Point origin = project({0.0f, 0.0f, 0.0f}, cx, cy, radius);
  for (int axis = 0; axis < 3; ++axis) {
    const Vec3 endpoint = core::imu_calibration::rotate(orientation, localAxes[axis]);
    const Point projected = project(endpoint, cx, cy, radius * 0.92f);
    canvas.drawLine(origin.x, origin.y, projected.x, projected.y, axisColor(axis));
    canvas.fillCircle(projected.x, projected.y, 3, axisColor(axis));
  }

  Vec3 corners[8];
  for (int i = 0; i < 8; ++i) {
    // The device is wider along pitch and longer along roll.
    const Vec3 local{(i & 1) ? 0.42f : -0.42f,
                     (i & 2) ? 0.68f : -0.68f,
                     (i & 4) ? 0.13f : -0.13f};
    corners[i] = core::imu_calibration::rotate(orientation, local);
  }
  static const uint8_t edges[][2] = {
      {0, 1}, {0, 2}, {1, 3}, {2, 3}, {4, 5}, {4, 6},
      {5, 7}, {6, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7},
  };
  for (const auto& edge : edges) {
    const Point a = project(corners[edge[0]], cx, cy, radius * 0.72f);
    const Point b = project(corners[edge[1]], cx, cy, radius * 0.72f);
    canvas.drawLine(a.x, a.y, b.x, b.y, kWhite);
  }

  const Vec3 rotation = s.tracker.rotationVectorDegrees();
  const Vec3 rates = s.tracker.ratesDps();
  char line[64];
  canvas.setTextColor(kWhite, kBg);
  snprintf(line, sizeof(line), "ROT    R%+4.0f P%+4.0f Y%+4.0f",
           rotation.x, rotation.y, rotation.z);
  ui::safe_area::drawCentreString(canvas, line, cx, 194, 1);
  canvas.setTextColor(kMuted, kBg);
  snprintf(line, sizeof(line), "RATE   R%+4.0f P%+4.0f Y%+4.0f",
           rates.x, rates.y, rates.z);
  ui::safe_area::drawCentreString(canvas, line, cx, 208, 1);
  ui::safe_area::drawCentreString(canvas, "B: RECENTER    A: HOME", cx, 244, 1);
}

void drawHeader(TFT_eSprite& canvas, const char* subtitle) {
  hal::display::clearContentArea(kBg);
  canvas.setTextColor(kWhite, kBg);
  canvas.setTextSize(2); ui::safe_area::drawString(canvas, "IMU CALIBRATION", 8, ui::safe_area::contentTop()+1, 1); canvas.setTextSize(1);
  canvas.setTextColor(kMuted, kBg);
  if (subtitle) ui::safe_area::drawString(canvas, subtitle, 8, ui::safe_area::contentTop()+19, 1);
}

void drawProgress(TFT_eSprite& canvas) {
  const int completed = s.axis * kTrialsPerAxis + s.trial;
  for (int i = 0; i < 9; ++i) {
    const int x = 30 + i * 21;
    const uint16_t color = i < completed ? kGreen :
                           (i == completed ? axisColor(s.axis) : kGrid);
    canvas.fillCircle(x, 66, 4, color);
  }
}

void draw(TFT_eSprite& canvas) {
  if (s.page == Page::Visualizer) {
    drawHeader(canvas, "RELATIVE ORIENTATION");
    drawVisualizer(canvas);
    return;
  }

  drawHeader(canvas, nullptr);
  canvas.setTextColor(kWhite, kBg);
  if (s.page == Page::Home) {
    canvas.setTextSize(2); ui::safe_area::drawString(canvas, "Default coordinate frame", 12, 67, 1); canvas.setTextSize(1);
    canvas.setTextColor(s.hasProfile ? kGreen : kYellow, kBg);
    canvas.setTextSize(2); ui::safe_area::drawString(canvas, s.hasProfile ? "PROFILE SAVED" : "NOT CALIBRATED", 12, 91, 1); canvas.setTextSize(1);
    const bool calibrationSelected = s.homeSelection == 0;
    canvas.fillRect(12, 126, 216, 34, calibrationSelected ? kCyan : kPanel);
    canvas.setTextColor(calibrationSelected ? kBg : kWhite,
                        calibrationSelected ? kCyan : kPanel);
    canvas.setTextSize(2); ui::safe_area::drawCentreString(canvas, "START CALIBRATION", 120, 135, 1); canvas.setTextSize(1);
    if (s.hasProfile) {
      canvas.fillRect(12, 168, 216, 34, !calibrationSelected ? kCyan : kPanel);
      canvas.setTextColor(!calibrationSelected ? kBg : kWhite,
                          !calibrationSelected ? kCyan : kPanel);
      canvas.setTextSize(2); ui::safe_area::drawCentreString(canvas, "OPEN VISUALIZER", 120, 177, 1); canvas.setTextSize(1);
    }
    canvas.setTextColor(kMuted, kBg);
    ui::safe_area::drawCentreString(canvas, "SCROLL: SELECT   C: OPEN", 120, 244, 1);
    return;
  }

  if (s.page == Page::Neutral) {
    canvas.setTextSize(2); ui::safe_area::drawCentreString(canvas, "NEUTRAL REFERENCE", 120, 67, 1); canvas.setTextSize(1);
    canvas.setTextColor(kMuted, kBg);
    ui::safe_area::drawCentreString(canvas, "Flat, face up, pointing forward", 120, 94, 1);
    const bool neutralSettling = s.neutralCollecting &&
        (int32_t)(millis() - s.neutralSettleUntilMs) < 0;
    canvas.setTextSize(2); ui::safe_area::drawCentreString(canvas, !s.neutralCollecting ? "Hold still, then press C" :
                            (neutralSettling ? "SETTLING" : "CAPTURING STABLE DATA"),
                            120, 112, 1); canvas.setTextSize(1);
    canvas.drawRect(30, 148, 180, 12, kGrid);
    const int neutralProgress = s.neutralCount > kNeutralSamples ? kNeutralSamples
                                                                 : s.neutralCount;
    canvas.fillRect(32, 150, (176 * neutralProgress) / kNeutralSamples, 8, kYellow);
    char raw[48];
    snprintf(raw, sizeof(raw), "A %.0f mg   G %.1f dps", s.lastAccelMagnitude,
             s.lastGyroMagnitude);
    ui::safe_area::drawCentreString(canvas, raw, 120, 177, 1);
  } else if (s.page == Page::ReturnNeutral || s.page == Page::Arming ||
             s.page == Page::Capture ||
             s.page == Page::TrialReview) {
    drawProgress(canvas);
    char heading[48];
    snprintf(heading, sizeof(heading), "%s  TRIAL %d/%d", axisName(s.axis),
             s.trial + 1, kTrialsPerAxis);
    canvas.setTextColor(axisColor(s.axis), kBg);
    canvas.setTextSize(2); ui::safe_area::drawCentreString(canvas, heading, 120, 83, 1); canvas.setTextSize(1);
    canvas.setTextColor(kWhite, kBg);
    if (s.page == Page::ReturnNeutral) {
      canvas.setTextSize(2); ui::safe_area::drawCentreString(canvas, "RETURN TO NEUTRAL", 120, 116, 1); canvas.setTextSize(1);
      canvas.setTextColor(s.neutralReady ? kGreen : kYellow, kBg);
      ui::safe_area::drawCentreString(canvas, s.neutralReady ? "READY - PRESS C" : "WAITING FOR STABLE POSE",
                              120, 142, 1);
    } else if (s.page == Page::Arming) {
      canvas.setTextSize(2); ui::safe_area::drawCentreString(canvas, "SETTLING", 120, 116, 1); canvas.setTextSize(1);
      canvas.setTextColor(kYellow, kBg);
      const bool buttonDelay = (int32_t)(millis() - s.armSettleUntilMs) < 0;
      ui::safe_area::drawCentreString(canvas, buttonDelay ? "Release the device" : "Hold neutral - GO is automatic",
                              120, 142, 1);
      char stable[48];
      snprintf(stable, sizeof(stable), "G %.1f dps  A %.0f mg",
               s.armGyroFiltered, s.armAccelFiltered);
      ui::safe_area::drawCentreString(canvas, stable, 120, 164, 1);
    } else if (s.page == Page::Capture) {
      canvas.setTextSize(2); ui::safe_area::drawCentreString(canvas, "RECORDING", 120, 116, 1); canvas.setTextSize(1);
      canvas.setTextColor(kYellow, kBg);
      ui::safe_area::drawCentreString(canvas, s.capture.motionStarted ? "Motion detected - finish and hold" :
                               "GO - rotate when ready", 120, 142, 1);
      char motion[48];
      snprintf(motion, sizeof(motion), "NET %.0f  TOTAL %.0f",
               core::imu_calibration::length(s.capture.rotation), s.capture.totalAngle);
      ui::safe_area::drawCentreString(canvas, motion, 120, 164, 1);
    } else {
      canvas.setTextColor(s.trialAccepted ? kGreen : kRed, kBg);
      canvas.setTextSize(2); ui::safe_area::drawCentreString(canvas, s.trialAccepted ? "TRIAL ACCEPTED" : "TRY AGAIN", 120, 116, 1); canvas.setTextSize(1);
      canvas.setTextColor(kWhite, kBg);
      ui::safe_area::drawCentreString(canvas, s.status, 120, 143, 1);
      canvas.setTextColor(kMuted, kBg);
      ui::safe_area::drawCentreString(canvas, "C: CONTINUE   A: REDO", 120, 174, 1);
    }
    if (s.page != Page::TrialReview) {
      canvas.setTextColor(kMuted, kBg);
      ui::safe_area::drawCentreString(canvas, s.status, 120, 195, 1);
      ui::safe_area::drawCentreString(canvas, "B: CANCEL", 120, 244, 1);
    }
  } else if (s.page == Page::Summary) {
    canvas.setTextSize(2); ui::safe_area::drawCentreString(canvas, "CALIBRATION SUMMARY", 120, 65, 1); canvas.setTextSize(1);
    for (int axis = 0; axis < 3; ++axis) {
      char line[64];
      snprintf(line, sizeof(line), "%s  %+.2f %+.2f %+.2f  %u%%", axisName(axis),
               s.profile.axes[axis][0], s.profile.axes[axis][1], s.profile.axes[axis][2],
               (unsigned)(s.profile.quality[axis] * 100.0f));
      canvas.setTextColor(axisColor(axis), kBg);
      ui::safe_area::drawString(canvas, line, 12, 101 + axis * 28, 1);
    }
    canvas.setTextColor(kMuted, kBg);
    ui::safe_area::drawCentreString(canvas, s.status, 120, 198, 1);
    ui::safe_area::drawCentreString(canvas, "C: SAVE + VISUALIZE   A: REDO YAW", 120, 244, 1);
  } else if (s.page == Page::Error) {
    canvas.setTextColor(kRed, kBg);
    canvas.setTextSize(2); ui::safe_area::drawCentreString(canvas, "CALIBRATION ERROR", 120, 92, 1); canvas.setTextSize(1);
    canvas.setTextColor(kWhite, kBg);
    ui::safe_area::drawCentreString(canvas, s.status, 120, 122, 1);
    canvas.setTextColor(kMuted, kBg);
    ui::safe_area::drawCentreString(canvas, "A: HOME", 120, 244, 1);
  }
}

void onInput(hal::input::InputEvent event) {
  using ET = hal::input::EventType;
  if (event.type == ET::Scroll && s.page == Page::Home && s.hasProfile) {
    s.homeSelection = event.delta > 0 ? 1 : 0;
    hal::display::markAllDirty();
    return;
  }

  if (event.type == ET::ButtonB_Tap) {
    if (s.page == Page::Visualizer) s.tracker.reset(s.profile);
    else if (s.page == Page::Neutral && s.neutralCollecting) {
      s.neutralCollecting = false;
      s.neutralCount = 0;
      resetStabilityWindow();
      setStatus("Neutral capture cancelled");
      Serial.println("[imu-cal] neutral result=cancelled");
      hal::display::markAllDirty();
    }
    else if (s.page == Page::Arming || s.page == Page::Capture) {
      Serial.printf("[imu-cal] axis=%s trial=%d result=cancelled\n",
                    axisName(s.axis), s.trial + 1);
      beginAxisStage(s.axis, s.trial);
    }
    return;
  }

  if (event.type == ET::ButtonA_Tap) {
    if (s.page == Page::Home) return;
    if (s.page == Page::Visualizer || s.page == Page::Neutral || s.page == Page::Error) {
      s.page = Page::Home;
    } else if (s.page == Page::TrialReview || s.page == Page::Arming ||
               s.page == Page::Capture ||
               s.page == Page::ReturnNeutral) {
      beginAxisStage(s.axis, s.trial);
      return;
    } else if (s.page == Page::Summary) {
      beginAxisStage(2, 0);
      return;
    }
    hal::display::markAllDirty();
    return;
  }

  if (event.type != ET::ButtonC_Tap) return;
  if (s.page == Page::Home) {
    if (s.homeSelection == 1 && s.hasProfile) enterVisualizer();
    else beginNeutral();
  } else if (s.page == Page::Neutral && !s.neutralCollecting) {
    s.neutralAccelSum = {0.0f, 0.0f, 0.0f};
    s.neutralGyroSum = {0.0f, 0.0f, 0.0f};
    s.neutralCount = 0;
    resetStabilityWindow();
    s.neutralSettleUntilMs = millis() + kButtonSettleMs;
    s.neutralCollecting = true;
    setStatus("Settling after button press");
    Serial.printf("[imu-cal] neutral stage=settling delay=%lums\n",
                  (unsigned long)kButtonSettleMs);
  } else if (s.page == Page::ReturnNeutral && s.neutralReady) {
    startArming();
  } else if (s.page == Page::TrialReview) {
    advanceAfterTrial();
  } else if (s.page == Page::Summary) {
    s.saveOk = core::imu_calibration::save(s.profile);
    s.hasProfile = s.saveOk;
    if (s.saveOk) enterVisualizer();
    else {
      s.page = Page::Error;
      setStatus("NVS profile save failed");
    }
  }
}

void buildScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  scene.addRect("imu-cal.bg", {0, 0, width, height}, kBg, 0, 0);
  scene.addLabel("imu-cal.title", {8, 22, (int16_t)(width - 16), 22},
                 "IMU Calibration", kWhite, kBg, 2,
                 ui::scene::TextAlign::TopLeft, 1);
}

static void enterView() { hal::display::markAllDirty(); }

const ::scene::Screen kScreen = {
    "IMU Calibration", enterView, nullptr, tick, onInput, nullptr, draw,
    "calibration", buildScene,
};

const ::scene::Screen* const kScreens[] = {&kScreen};

}  // namespace

// Resume deliberately resets the workflow as the former screen re-entry did.
App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("IMU Calibration", "imu-calibration", &kScreen,
                            enter, enter, exitApp,
                            "src/apps/imu_calibration_app.cpp", kScreens, nullptr, nullptr);

}  // namespace imu_calibration_app
}  // namespace apps

#endif  // MENU_APP
