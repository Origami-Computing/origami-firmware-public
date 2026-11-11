#include "services/imu_sensor_calibration.h"

#include <Arduino.h>
#include <Preferences.h>

#include "hal/hal_imu.h"

namespace services {
namespace imu_sensor_calibration {

namespace {

namespace cal = core::imu_sensor_calibration;

constexpr char kNamespace[] = "imu_sens";
constexpr char kKey[] = "record";
constexpr uint32_t kSettleMs = 600;       // gyro turn-on + filter settling after the lease
constexpr uint32_t kSamplePeriodMs = 5;   // gyro ODR is 208 Hz while leased

Record s_active{};
bool s_hasActive = false;

bool readStored(Record* out) {
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  const size_t length = prefs.getBytesLength(kKey);
  const size_t read = length == sizeof(Record) ? prefs.getBytes(kKey, out, sizeof(Record)) : 0;
  prefs.end();
  return read == sizeof(Record) && cal::isValid(*out);
}

void install(const Record* record) {
  if (record) {
    s_active = *record;
    s_hasActive = true;
    hal::imu::setCorrection(&record->correction);
  } else {
    s_hasActive = false;
    hal::imu::setCorrection(nullptr);
  }
}

}  // namespace

bool loadAndApply() {
  Record record{};
  if (!readStored(&record)) {
    install(nullptr);
    Serial.println("[imu-cal] no stored sensor calibration; using raw IMU readings");
    return false;
  }
  install(&record);
  const auto& c = record.correction;
  Serial.printf("[imu-cal] loaded faces=0x%02X full=0x%X gyro_bias_mdps=%.0f,%.0f,%.0f "
                "accel_offset_mg=%.1f,%.1f,%.1f accel_scale=%.4f,%.4f,%.4f\n",
                record.faces, record.fullAxes, c.gyroBiasMdps[0], c.gyroBiasMdps[1],
                c.gyroBiasMdps[2], c.accelOffsetMg[0], c.accelOffsetMg[1], c.accelOffsetMg[2],
                c.accelScale[0], c.accelScale[1], c.accelScale[2]);
  return true;
}

bool active(Record* out) {
  if (s_hasActive && out) *out = s_active;
  return s_hasActive;
}

bool clear() {
  install(nullptr);
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return false;
  if (prefs.isKey(kKey)) prefs.remove(kKey);
  const bool gone = !prefs.isKey(kKey);
  prefs.end();
  return gone;
}

SaveResult solveAndSave(const PoseSet& poses, Solution* outSolution) {
  if (cal::poseCount(poses) == 0) return SaveResult::NoPoses;
  Solution solution{};
  if (!cal::solve(poses, &solution)) return SaveResult::Implausible;
  if (outSolution) *outSolution = solution;
  const Record record = cal::makeRecord(solution);
  Preferences prefs;
  if (!prefs.begin(kNamespace, false)) return SaveResult::WriteFailed;
  const size_t written = prefs.putBytes(kKey, &record, sizeof(record));
  prefs.end();
  Record readBack{};
  if (written != sizeof(record) || !readStored(&readBack) ||
      readBack.checksum != record.checksum) return SaveResult::WriteFailed;
  install(&readBack);
  return SaveResult::Saved;
}

// ---------------------------------------------------------------------------

bool Capture::begin(uint32_t nowMs, uint32_t timeoutMs) {
  cancel();
  averager_.reset();
  pose_ = Pose{};
  face_ = -1;
  failure_ = Failure::None;
  if (!hal::imu::isReady() || !gyro_.open()) {
    finish(State::Failed, Failure::Unavailable);
    return false;
  }
  state_ = State::Settling;
  startMs_ = nowMs;
  timeoutMs_ = timeoutMs;
  lastSampleMs_ = nowMs;
  return true;
}

Capture::State Capture::step(uint32_t nowMs) {
  if (state_ != State::Settling && state_ != State::Sampling) return state_;
  if ((uint32_t)(nowMs - startMs_) >= timeoutMs_) {
    finish(State::Failed, Failure::Timeout);
    return state_;
  }
  if (state_ == State::Settling) {
    if ((uint32_t)(nowMs - startMs_) < kSettleMs) return state_;
    state_ = State::Sampling;
  }
  if ((uint32_t)(nowMs - lastSampleMs_) < kSamplePeriodMs) return state_;
  lastSampleMs_ = nowMs;
  int32_t accel[3] = {}, gyro[3] = {};
  if (!hal::imu::readRawAxes(accel, gyro)) return state_;  // transient bus error: retry
  const float a[3] = {(float)accel[0], (float)accel[1], (float)accel[2]};
  const float g[3] = {(float)gyro[0], (float)gyro[1], (float)gyro[2]};
  if (averager_.add(a, g) != cal::StillAverager::Step::Complete) return state_;
  pose_ = averager_.pose();
  face_ = cal::faceOf(pose_.accelMg);
  if (face_ < 0) finish(State::Failed, Failure::NotAxisAligned);
  else finish(State::Done, Failure::None);
  return state_;
}

void Capture::cancel() {
  gyro_.close();
  if (state_ == State::Settling || state_ == State::Sampling) state_ = State::Idle;
}

void Capture::finish(State state, Failure failure) {
  gyro_.close();
  state_ = state;
  failure_ = failure;
}

const char* stateName(Capture::State state) {
  switch (state) {
    case Capture::State::Idle: return "idle";
    case Capture::State::Settling: return "settling";
    case Capture::State::Sampling: return "sampling";
    case Capture::State::Done: return "done";
    case Capture::State::Failed: return "failed";
  }
  return "?";
}

const char* failureName(Capture::Failure failure) {
  switch (failure) {
    case Capture::Failure::None: return "none";
    case Capture::Failure::Unavailable: return "imu-unavailable";
    case Capture::Failure::Timeout: return "timeout-moving";
    case Capture::Failure::NotAxisAligned: return "not-axis-aligned";
  }
  return "?";
}

const char* saveResultName(SaveResult result) {
  switch (result) {
    case SaveResult::Saved: return "saved";
    case SaveResult::NoPoses: return "no-poses";
    case SaveResult::Implausible: return "implausible";
    case SaveResult::WriteFailed: return "write-failed";
  }
  return "?";
}

}  // namespace imu_sensor_calibration
}  // namespace services
