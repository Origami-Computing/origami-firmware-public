#include "tooling/imu_calibration_commands.h"
#ifdef ORIGAMI_DEV_SERIAL_INPUT
#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hal/hal_imu.h"
#include "services/imu_sensor_calibration.h"
#include "services/motion.h"

// Commands (all print a "[imu-cal]" line):
//   imu cal              capture this orientation, solve and save (single pose)
//   imu cal pose         capture this orientation into the pending pose set
//   imu cal save         solve and save the pending pose set (up to 6 faces)
//   imu cal reset        discard pending poses
//   imu cal cancel       abort a running capture or stream
//   imu cal status       show the installed calibration and pending poses
//   imu cal clear        delete the stored calibration (raw readings resume)
//   imu stream [S] [raw] corrected (or raw) samples for S seconds + statistics
namespace tooling { namespace imu_calibration_commands {
namespace {
namespace svc = services::imu_sensor_calibration;
namespace cal = core::imu_sensor_calibration;

svc::Capture s_capture;
cal::PoseSet s_pending{};
bool s_saveAfterCapture = false;
svc::Capture::State s_lastState = svc::Capture::State::Idle;
int s_lastWindows = 0;
uint32_t s_lastMoved = 0;

struct Stream {
  bool active = false;
  bool raw = false;
  uint32_t untilMs = 0, nextSampleMs = 0, nextPrintMs = 0;
  uint32_t n = 0;
  double sum[6] = {}, sq[6] = {};
  services::motion::Gyroscope gyro;
  uint32_t settleUntilMs = 0;
} s_stream;

void printCorrection(const char* tag, const cal::Correction& c) {
  Serial.printf("[imu-cal] %s gyro_bias_mdps=%.1f,%.1f,%.1f accel_offset_mg=%.2f,%.2f,%.2f "
                "accel_scale=%.5f,%.5f,%.5f\n", tag,
                c.gyroBiasMdps[0], c.gyroBiasMdps[1], c.gyroBiasMdps[2],
                c.accelOffsetMg[0], c.accelOffsetMg[1], c.accelOffsetMg[2],
                c.accelScale[0], c.accelScale[1], c.accelScale[2]);
}

void printPendingFaces() {
  Serial.printf("[imu-cal] pending poses=%d faces=", cal::poseCount(s_pending));
  for (int f = 0; f < 6; ++f) if (s_pending.has[f]) Serial.printf("%s ", cal::faceName(f));
  Serial.println();
}

void printStatus() {
  svc::Record r{};
  if (svc::active(&r)) {
    Serial.printf("[imu-cal] status=calibrated faces=0x%02X full_axes=0x%X samples=%lu "
                  "residual_mg=%.2f gyro_sd_mdps=%.1f accel_sd_mg=%.2f\n",
                  r.faces, r.fullAxes, (unsigned long)r.samples, r.maxResidualMg,
                  r.gyroStdMdps, r.accelStdMg);
    printCorrection("installed", r.correction);
  } else {
    Serial.println("[imu-cal] status=uncalibrated (raw readings)");
  }
  printPendingFaces();
  Serial.printf("[imu-cal] capture=%s\n", svc::stateName(s_capture.state()));
}

void startCapture(uint32_t now, bool saveAfter) {
  if (s_stream.active) { Serial.println("[imu-cal] ERR busy-streaming"); return; }
  if (s_capture.state() == svc::Capture::State::Settling ||
      s_capture.state() == svc::Capture::State::Sampling) {
    Serial.println("[imu-cal] ERR busy-capturing");
    return;
  }
  if (saveAfter) cal::clear(&s_pending);
  s_saveAfterCapture = saveAfter;
  s_lastWindows = 0;
  s_lastMoved = 0;
  if (!s_capture.begin(now)) {
    s_lastState = s_capture.state();
    Serial.printf("[imu-cal] capture result=failed reason=%s\n",
                  svc::failureName(s_capture.failure()));
    return;
  }
  s_lastState = s_capture.state();
  Serial.println("[imu-cal] capture start: keep the device still");
}

void save() {
  svc::Solution solution{};
  const svc::SaveResult result = svc::solveAndSave(s_pending, &solution);
  Serial.printf("[imu-cal] save result=%s\n", svc::saveResultName(result));
  if (result != svc::SaveResult::Saved) return;
  Serial.printf("[imu-cal] solved faces=0x%02X full_axes=0x%X samples=%lu residual_mg=%.2f\n",
                solution.faces, solution.fullAxes, (unsigned long)solution.samples,
                solution.maxResidualMg);
  printCorrection("saved", solution.correction);
}

void finishCapture() {
  const auto state = s_capture.state();
  if (state == svc::Capture::State::Failed) {
    Serial.printf("[imu-cal] capture result=failed reason=%s moved_windows=%lu\n",
                  svc::failureName(s_capture.failure()), (unsigned long)s_capture.movedWindows());
    return;
  }
  const cal::Pose& p = s_capture.pose();
  int face = -1;
  cal::addPose(&s_pending, p, &face);
  Serial.printf("[imu-cal] capture result=ok face=%s samples=%lu moved_windows=%lu "
                "accel_mg=%.2f,%.2f,%.2f accel_sd_mg=%.2f gyro_mdps=%.1f,%.1f,%.1f "
                "gyro_sd_mdps=%.1f\n",
                cal::faceName(face), (unsigned long)p.samples,
                (unsigned long)s_capture.movedWindows(), p.accelMg[0], p.accelMg[1],
                p.accelMg[2], p.accelStdMg, p.gyroMdps[0], p.gyroMdps[1], p.gyroMdps[2],
                p.gyroStdMdps);
  printPendingFaces();
  if (s_saveAfterCapture) save();
}

void startStream(uint32_t now, float seconds, bool raw) {
  if (s_capture.state() == svc::Capture::State::Settling ||
      s_capture.state() == svc::Capture::State::Sampling || s_stream.active) {
    Serial.println("[imu-cal] ERR busy");
    return;
  }
  if (!hal::imu::isReady() || !s_stream.gyro.open()) {
    Serial.println("[imu-cal] ERR imu-unavailable");
    return;
  }
  s_stream.active = true;
  s_stream.raw = raw;
  s_stream.n = 0;
  memset(s_stream.sum, 0, sizeof(s_stream.sum));
  memset(s_stream.sq, 0, sizeof(s_stream.sq));
  s_stream.settleUntilMs = now + 600;
  s_stream.untilMs = s_stream.settleUntilMs + (uint32_t)(seconds * 1000.0f);
  s_stream.nextSampleMs = s_stream.settleUntilMs;
  s_stream.nextPrintMs = s_stream.settleUntilMs;
  Serial.printf("[imu-stream] start mode=%s seconds=%.1f\n", raw ? "raw" : "corrected", seconds);
}

void endStream(bool printSummary) {
  s_stream.gyro.close();
  s_stream.active = false;
  if (!printSummary || !s_stream.n) return;
  double mean[6], sd[6];
  for (int i = 0; i < 6; ++i) {
    mean[i] = s_stream.sum[i] / s_stream.n;
    sd[i] = sqrt(fmax(0.0, s_stream.sq[i] / s_stream.n - mean[i] * mean[i]));
  }
  const double norm = sqrt(mean[0] * mean[0] + mean[1] * mean[1] + mean[2] * mean[2]);
  Serial.printf("[imu-stream] summary mode=%s n=%lu accel_mean_mg=%.2f,%.2f,%.2f |a|=%.2f "
                "accel_sd_mg=%.2f,%.2f,%.2f gyro_mean_dps=%.4f,%.4f,%.4f "
                "gyro_sd_dps=%.4f,%.4f,%.4f\n",
                s_stream.raw ? "raw" : "corrected", (unsigned long)s_stream.n, mean[0], mean[1],
                mean[2], norm, sd[0], sd[1], sd[2], mean[3] / 1000.0, mean[4] / 1000.0,
                mean[5] / 1000.0, sd[3] / 1000.0, sd[4] / 1000.0, sd[5] / 1000.0);
}

void pumpStream(uint32_t now) {
  if (!s_stream.active) return;
  if ((int32_t)(now - s_stream.untilMs) >= 0) { endStream(true); return; }
  if ((int32_t)(now - s_stream.nextSampleMs) < 0) return;
  s_stream.nextSampleMs = now + 5;
  int32_t a[3] = {}, g[3] = {};
  const bool ok = s_stream.raw ? hal::imu::readRawAxes(a, g) : hal::imu::readAxes(a, g);
  if (!ok) return;
  const int32_t v[6] = {a[0], a[1], a[2], g[0], g[1], g[2]};
  for (int i = 0; i < 6; ++i) {
    s_stream.sum[i] += v[i];
    s_stream.sq[i] += (double)v[i] * v[i];
  }
  ++s_stream.n;
  if ((int32_t)(now - s_stream.nextPrintMs) >= 0) {
    s_stream.nextPrintMs = now + 250;
    Serial.printf("[imu-stream] t=%lu accel_mg=%ld,%ld,%ld gyro_mdps=%ld,%ld,%ld\n",
                  (unsigned long)now, (long)a[0], (long)a[1], (long)a[2], (long)g[0],
                  (long)g[1], (long)g[2]);
  }
}
}  // namespace

bool handleCommand(const char* line, uint32_t now) {
  if (!strcmp(line, "imu cal")) { startCapture(now, true); return true; }
  if (!strcmp(line, "imu cal pose")) { startCapture(now, false); return true; }
  if (!strcmp(line, "imu cal save")) { save(); return true; }
  if (!strcmp(line, "imu cal reset")) { cal::clear(&s_pending); printPendingFaces(); return true; }
  if (!strcmp(line, "imu cal status")) { printStatus(); return true; }
  if (!strcmp(line, "imu cal clear")) {
    Serial.printf("[imu-cal] clear result=%s\n", services::imu_sensor_calibration::clear() ? "ok" : "failed");
    return true;
  }
  if (!strcmp(line, "imu cal cancel")) {
    s_capture.cancel();
    endStream(false);
    Serial.println("[imu-cal] cancelled");
    return true;
  }
  if (!strncmp(line, "imu stream", 10) && (line[10] == 0 || line[10] == ' ')) {
    float seconds = 5.0f;
    char mode[8] = {};
    const int fields = sscanf(line + 10, "%f %7s", &seconds, mode);
    if (fields < 1) seconds = 5.0f;
    if (!(seconds > 0.0f && seconds <= 120.0f) || (fields == 2 && strcmp(mode, "raw"))) {
      Serial.println("[imu-cal] usage: imu stream [1..120] [raw]");
      return true;
    }
    startStream(now, seconds, fields == 2);
    return true;
  }
  return false;
}

void service(uint32_t now) {
  pumpStream(now);
  const auto state = s_capture.step(now);
  if (state == svc::Capture::State::Sampling &&
      (s_capture.acceptedWindows() != s_lastWindows || s_capture.movedWindows() != s_lastMoved)) {
    if (s_capture.movedWindows() != s_lastMoved) {
      const auto& w = s_capture.averager();
      Serial.printf("[imu-cal] window=moved gyro_sd_mdps=%.1f accel_sd_mg=%.2f gyro_drift_mdps=%.1f "
                    "accel_drift_mg=%.2f |a|=%.1f (restarting)\n", w.lastGyroStdMdps(),
                    w.lastAccelStdMg(), w.lastGyroDriftMdps(), w.lastAccelDriftMg(),
                    w.lastAccelNormMg());
    }
    s_lastWindows = s_capture.acceptedWindows();
    s_lastMoved = s_capture.movedWindows();
  }
  if (state == s_lastState) return;
  s_lastState = state;
  if (state == svc::Capture::State::Done || state == svc::Capture::State::Failed) finishCapture();
}

} }
#else
namespace tooling { namespace imu_calibration_commands {
bool handleCommand(const char*, uint32_t) { return false; }
void service(uint32_t) {}
} }
#endif
