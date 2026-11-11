#pragma once
/**
 * Persistent IMU sensor calibration (gyro zero-rate bias, accelerometer
 * offset/scale). This service owns the NVS schema ("imu_sens"/"record",
 * core::imu_sensor_calibration::Record), validation, the stillness-gated
 * capture procedure and installing the correction into hal::imu, so every
 * consumer of hal::imu::readAxes/readAcceleration/readGyroFifo (games, home
 * eyes, Doodle, the frame visualizer) receives corrected samples without
 * knowing about calibration.
 *
 * Threading: UI task only. No worker task; a Capture is stepped by its owner.
 */
#include <stdint.h>

#include "core/imu_sensor_calibration.h"
#include "services/motion.h"

namespace services {
namespace imu_sensor_calibration {

using core::imu_sensor_calibration::Pose;
using core::imu_sensor_calibration::PoseSet;
using core::imu_sensor_calibration::Record;
using core::imu_sensor_calibration::Solution;

/** Boot: load the stored record and install it (identity when missing or
 * invalid). Returns true when a valid stored calibration was installed. */
bool loadAndApply();
/** Copy of the installed record; false when running uncalibrated. */
bool active(Record* out);
/** Remove the stored record and install identity. True when nothing remains. */
bool clear();

enum class SaveResult : uint8_t { Saved, NoPoses, Implausible, WriteFailed };
/** Solve from the captured poses, persist, read back and install. On any
 * failure the previously installed correction stays in effect. */
SaveResult solveAndSave(const PoseSet& poses, Solution* outSolution = nullptr);

/**
 * One stillness-gated capture in the current orientation. Holds a gyroscope
 * lease while running (released on finish/cancel/destruction). Samples raw
 * (uncorrected) axes at most every 5 ms from step(); after a 600 ms settle it
 * needs `requiredWindows` consecutive still windows. Any window with motion
 * (variance, drift or |g| out of range) restarts the capture; it fails after
 * `timeoutMs` without completing.
 */
class Capture {
 public:
  enum class State : uint8_t { Idle, Settling, Sampling, Done, Failed };
  enum class Failure : uint8_t { None, Unavailable, Timeout, NotAxisAligned };

  Capture() = default;
  ~Capture() { cancel(); }
  Capture(const Capture&) = delete;
  Capture& operator=(const Capture&) = delete;

  /** False (Failed/Unavailable) when the IMU or gyro is unavailable. */
  bool begin(uint32_t nowMs, uint32_t timeoutMs = 60000);
  /** Bounded: at most one raw IMU read per call. */
  State step(uint32_t nowMs);
  void cancel();

  State state() const { return state_; }
  Failure failure() const { return failure_; }
  /** Valid in Done. */
  const Pose& pose() const { return pose_; }
  int face() const { return face_; }
  uint32_t movedWindows() const { return averager_.rejectedWindows(); }
  int acceptedWindows() const { return averager_.acceptedWindows(); }
  const core::imu_sensor_calibration::StillAverager& averager() const { return averager_; }

 private:
  void finish(State state, Failure failure);
  services::motion::Gyroscope gyro_;
  core::imu_sensor_calibration::StillAverager averager_;
  State state_ = State::Idle;
  Failure failure_ = Failure::None;
  Pose pose_{};
  int face_ = -1;
  uint32_t startMs_ = 0;
  uint32_t timeoutMs_ = 0;
  uint32_t lastSampleMs_ = 0;
};

const char* stateName(Capture::State state);
const char* failureName(Capture::Failure failure);
const char* saveResultName(SaveResult result);

}  // namespace imu_sensor_calibration
}  // namespace services
