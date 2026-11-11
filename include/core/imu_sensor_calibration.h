#pragma once
/**
 * Sensor-level LSM6DSL calibration math: gyroscope zero-rate bias and
 * accelerometer per-axis offset/scale, estimated from stillness-gated poses.
 *
 * This is distinct from core/imu_calibration.h, which maps the (already
 * corrected) sensor frame onto the IMU visualizer's roll/pitch/yaw axes.
 *
 * Pure C++ (no Arduino); persisted and applied by
 * services/imu_sensor_calibration.h, consumed through hal::imu reads.
 * Host test: test/test_imu_sensor_calibration.
 */
#include <stdint.h>

namespace core {
namespace imu_sensor_calibration {

/** corrected accel = (raw - accelOffsetMg) * accelScale; gyro = raw - gyroBiasMdps. */
struct Correction {
  float gyroBiasMdps[3];
  float accelOffsetMg[3];
  float accelScale[3];
};

Correction identity();
bool isPlausible(const Correction& correction);
void applyAccel(const Correction& correction, int32_t mg[3]);
void applyGyro(const Correction& correction, int32_t mdps[3]);

/** Mean of a stillness-gated capture in one orientation (raw sensor units). */
struct Pose {
  float accelMg[3];
  float gyroMdps[3];
  float accelStdMg;    // largest per-axis standard deviation in the capture
  float gyroStdMdps;
  uint32_t samples;
};

/** Faces: 0=+X 1=-X 2=+Y 3=-Y 4=+Z 5=-Z (the axis that reads +1 g / -1 g).
 * Returns -1 when gravity is not within ~10 degrees of a sensor axis. */
int faceOf(const float accelMg[3]);
const char* faceName(int face);

struct StillnessLimits {
  int windowSamples = 26;
  int requiredWindows = 10;
  // Per-axis std within one window. Device noise at rest is ~70-100 mdps and
  // ~2-4 mg; desk typing reaches ~6-9 mg, a hand-held device far more.
  float maxGyroStdMdps = 300.0f;
  float maxAccelStdMg = 10.0f;
  float maxGyroDriftMdps = 150.0f; // window mean vs running mean (slow turns)
  float maxAccelDriftMg = 6.0f;    // window mean vs running mean (slow tilts)
  float minAccelNormMg = 900.0f;
  float maxAccelNormMg = 1100.0f;
};

/** Accumulates consecutive still windows; any moving window restarts the
 * capture. Not thread-safe; owned by one sampling session. */
class StillAverager {
 public:
  enum class Step : uint8_t { Collecting, WindowAccepted, Moved, Complete };
  explicit StillAverager(const StillnessLimits& limits = StillnessLimits());
  void reset();
  Step add(const float accelMg[3], const float gyroMdps[3]);
  bool complete() const { return windows_ >= limits_.requiredWindows; }
  int acceptedWindows() const { return windows_; }
  uint32_t rejectedWindows() const { return rejected_; }
  /** Valid after Complete. */
  Pose pose() const;
  /** Diagnostics of the last finished window. */
  float lastGyroStdMdps() const { return lastGyroStd_; }
  float lastAccelStdMg() const { return lastAccelStd_; }
  float lastGyroDriftMdps() const { return lastGyroDrift_; }
  float lastAccelDriftMg() const { return lastAccelDrift_; }
  float lastAccelNormMg() const { return lastNorm_; }

 private:
  void restartWindow();
  StillnessLimits limits_;
  // current window
  int n_;
  double wSum_[6], wSq_[6];
  // accepted windows
  int windows_;
  uint32_t rejected_;
  double sum_[6];
  double sq_[6];
  uint32_t samples_;
  float lastGyroStd_ = 0, lastAccelStd_ = 0, lastGyroDrift_ = 0, lastAccelDrift_ = 0, lastNorm_ = 0;
};

/** One captured pose per face. */
struct PoseSet {
  Pose poses[6];
  bool has[6];
};
void clear(PoseSet* set);
/** Stores the pose under its face; false (unchanged) when not axis-aligned. */
bool addPose(PoseSet* set, const Pose& pose, int* outFace = nullptr);
int poseCount(const PoseSet& set);

struct Solution {
  Correction correction;
  uint8_t fullAxes;        // bit k: +k and -k both captured -> offset+scale solved
  uint8_t faces;           // bit f: face f captured
  float maxResidualMg;     // max | |corrected pose| - 1000 mg | over captured poses
  float gyroStdMdps;       // worst capture noise
  float accelStdMg;
  uint32_t samples;
};

/** Gyro bias = sample-weighted mean of all poses. Accel per axis: both faces ->
 * offset/scale; one face on the gravity axis -> offset so |g| = 1 g with scale
 * 1 (the two perpendicular axes stay uncorrected, since a desk tilt cannot be
 * separated from offset with one pose). Fails for an empty set or an
 * implausible result. */
bool solve(const PoseSet& set, Solution* out);

/** Versioned NVS record. */
struct Record {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  Correction correction;
  uint8_t fullAxes;
  uint8_t faces;
  uint16_t reserved;
  float maxResidualMg;
  float gyroStdMdps;
  float accelStdMg;
  uint32_t samples;
  uint32_t checksum;
};

Record makeRecord(const Solution& solution);
bool isValid(const Record& record);

}  // namespace imu_sensor_calibration
}  // namespace core
