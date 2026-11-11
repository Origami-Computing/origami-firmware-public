#include "core/imu_sensor_calibration.h"

#include <math.h>
#include <string.h>

namespace core {
namespace imu_sensor_calibration {

namespace {

constexpr uint32_t kMagic = 0x494D5353u;  // "IMSS"
constexpr uint16_t kVersion = 1;
constexpr float kGravityMg = 1000.0f;
constexpr float kMaxGyroBiasMdps = 15000.0f;  // LSM6DSL zero-rate level is a few dps
constexpr float kMaxAccelOffsetMg = 150.0f;   // datasheet typ. +-40 mg, plus solder stress
constexpr float kMinScale = 0.9f;
constexpr float kMaxScale = 1.1f;
constexpr float kAxisAlignment = 0.985f;      // cos(10 degrees)

uint32_t checksum(const Record& source) {
  Record record = source;
  record.checksum = 0;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&record);
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < sizeof(record); ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

int32_t roundToInt(float value) { return (int32_t)lroundf(value); }

}  // namespace

Correction identity() {
  Correction c{};
  for (int k = 0; k < 3; ++k) c.accelScale[k] = 1.0f;
  return c;
}

bool isPlausible(const Correction& c) {
  for (int k = 0; k < 3; ++k) {
    if (!isfinite(c.gyroBiasMdps[k]) || !isfinite(c.accelOffsetMg[k]) ||
        !isfinite(c.accelScale[k])) return false;
    if (fabsf(c.gyroBiasMdps[k]) > kMaxGyroBiasMdps) return false;
    if (fabsf(c.accelOffsetMg[k]) > kMaxAccelOffsetMg) return false;
    if (c.accelScale[k] < kMinScale || c.accelScale[k] > kMaxScale) return false;
  }
  return true;
}

void applyAccel(const Correction& c, int32_t mg[3]) {
  for (int k = 0; k < 3; ++k)
    mg[k] = roundToInt(((float)mg[k] - c.accelOffsetMg[k]) * c.accelScale[k]);
}

void applyGyro(const Correction& c, int32_t mdps[3]) {
  for (int k = 0; k < 3; ++k) mdps[k] = roundToInt((float)mdps[k] - c.gyroBiasMdps[k]);
}

int faceOf(const float a[3]) {
  const float norm = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  if (!(norm > 1.0f)) return -1;
  for (int k = 0; k < 3; ++k) {
    if (fabsf(a[k]) >= kAxisAlignment * norm) return k * 2 + (a[k] < 0.0f ? 1 : 0);
  }
  return -1;
}

const char* faceName(int face) {
  static const char* const kNames[6] = {"+X", "-X", "+Y", "-Y", "+Z", "-Z"};
  return face >= 0 && face < 6 ? kNames[face] : "none";
}

// ---------------------------------------------------------------------------

StillAverager::StillAverager(const StillnessLimits& limits) : limits_(limits) { reset(); }

void StillAverager::reset() {
  restartWindow();
  windows_ = 0;
  rejected_ = 0;
  samples_ = 0;
  memset(sum_, 0, sizeof(sum_));
  memset(sq_, 0, sizeof(sq_));
}

void StillAverager::restartWindow() {
  n_ = 0;
  memset(wSum_, 0, sizeof(wSum_));
  memset(wSq_, 0, sizeof(wSq_));
}

StillAverager::Step StillAverager::add(const float accelMg[3], const float gyroMdps[3]) {
  if (complete()) return Step::Complete;
  for (int k = 0; k < 3; ++k) {
    wSum_[k] += accelMg[k];
    wSq_[k] += (double)accelMg[k] * accelMg[k];
    wSum_[k + 3] += gyroMdps[k];
    wSq_[k + 3] += (double)gyroMdps[k] * gyroMdps[k];
  }
  if (++n_ < limits_.windowSamples) return Step::Collecting;

  double mean[6];
  float accelStd = 0.0f, gyroStd = 0.0f, accelDrift = 0.0f, gyroDrift = 0.0f;
  for (int i = 0; i < 6; ++i) {
    mean[i] = wSum_[i] / n_;
    const float sd = (float)sqrt(fmax(0.0, wSq_[i] / n_ - mean[i] * mean[i]));
    const float drift = samples_ ? (float)fabs(mean[i] - sum_[i] / samples_) : 0.0f;
    if (i < 3) {
      accelStd = fmaxf(accelStd, sd);
      accelDrift = fmaxf(accelDrift, drift);
    } else {
      gyroStd = fmaxf(gyroStd, sd);
      gyroDrift = fmaxf(gyroDrift, drift);
    }
  }
  const float norm = (float)sqrt(mean[0] * mean[0] + mean[1] * mean[1] + mean[2] * mean[2]);
  lastAccelStd_ = accelStd;
  lastGyroStd_ = gyroStd;
  lastAccelDrift_ = accelDrift;
  lastGyroDrift_ = gyroDrift;
  lastNorm_ = norm;
  const bool still = accelStd <= limits_.maxAccelStdMg && gyroStd <= limits_.maxGyroStdMdps &&
                     accelDrift <= limits_.maxAccelDriftMg &&
                     gyroDrift <= limits_.maxGyroDriftMdps &&
                     norm >= limits_.minAccelNormMg && norm <= limits_.maxAccelNormMg;
  if (!still) {
    // Consecutive stillness is required: discard everything accepted so far.
    const uint32_t rejected = rejected_ + 1;
    reset();
    rejected_ = rejected;
    return Step::Moved;
  }
  for (int i = 0; i < 6; ++i) {
    sum_[i] += wSum_[i];
    sq_[i] += wSq_[i];
  }
  samples_ += (uint32_t)n_;
  ++windows_;
  restartWindow();
  return complete() ? Step::Complete : Step::WindowAccepted;
}

Pose StillAverager::pose() const {
  Pose p{};
  p.samples = samples_;
  if (!samples_) return p;
  for (int i = 0; i < 6; ++i) {
    const double mean = sum_[i] / samples_;
    const float sd = (float)sqrt(fmax(0.0, sq_[i] / samples_ - mean * mean));
    if (i < 3) {
      p.accelMg[i] = (float)mean;
      p.accelStdMg = fmaxf(p.accelStdMg, sd);
    } else {
      p.gyroMdps[i - 3] = (float)mean;
      p.gyroStdMdps = fmaxf(p.gyroStdMdps, sd);
    }
  }
  return p;
}

// ---------------------------------------------------------------------------

void clear(PoseSet* set) {
  if (set) memset(set, 0, sizeof(*set));
}

bool addPose(PoseSet* set, const Pose& pose, int* outFace) {
  const int face = faceOf(pose.accelMg);
  if (outFace) *outFace = face;
  if (!set || face < 0 || pose.samples == 0) return false;
  set->poses[face] = pose;
  set->has[face] = true;
  return true;
}

int poseCount(const PoseSet& set) {
  int count = 0;
  for (bool has : set.has) count += has ? 1 : 0;
  return count;
}

bool solve(const PoseSet& set, Solution* out) {
  if (!out || poseCount(set) == 0) return false;
  Solution s{};
  s.correction = identity();

  double gyroSum[3] = {0, 0, 0};
  for (int f = 0; f < 6; ++f) {
    if (!set.has[f]) continue;
    const Pose& p = set.poses[f];
    s.faces |= (uint8_t)(1u << f);
    s.samples += p.samples;
    s.gyroStdMdps = fmaxf(s.gyroStdMdps, p.gyroStdMdps);
    s.accelStdMg = fmaxf(s.accelStdMg, p.accelStdMg);
    for (int k = 0; k < 3; ++k) gyroSum[k] += (double)p.gyroMdps[k] * p.samples;
  }
  if (!s.samples) return false;
  for (int k = 0; k < 3; ++k) s.correction.gyroBiasMdps[k] = (float)(gyroSum[k] / s.samples);

  // Opposite faces: the mean is the offset and the half-difference is 1 g.
  for (int k = 0; k < 3; ++k) {
    if (!set.has[2 * k] || !set.has[2 * k + 1]) continue;
    const float plus = set.poses[2 * k].accelMg[k];
    const float minus = set.poses[2 * k + 1].accelMg[k];
    if (!(plus - minus > 1.0f)) return false;
    s.correction.accelOffsetMg[k] = 0.5f * (plus + minus);
    s.correction.accelScale[k] = 2.0f * kGravityMg / (plus - minus);
    s.fullAxes |= (uint8_t)(1u << k);
  }
  // A single face on an otherwise unsolved axis: offset such that |g| = 1 g.
  for (int f = 0; f < 6; ++f) {
    const int k = f / 2;
    if (!set.has[f] || (s.fullAxes & (1u << k))) continue;
    const Pose& p = set.poses[f];
    float perpendicular = 0.0f;
    for (int j = 0; j < 3; ++j) {
      if (j == k) continue;
      const float c = (p.accelMg[j] - s.correction.accelOffsetMg[j]) * s.correction.accelScale[j];
      perpendicular += c * c;
    }
    const float along = kGravityMg * kGravityMg - perpendicular;
    if (along <= 0.0f) return false;
    const float sign = (f & 1) ? -1.0f : 1.0f;
    s.correction.accelOffsetMg[k] = p.accelMg[k] - sign * sqrtf(along);
  }
  if (!isPlausible(s.correction)) return false;

  for (int f = 0; f < 6; ++f) {
    if (!set.has[f]) continue;
    float norm = 0.0f;
    for (int k = 0; k < 3; ++k) {
      const float c = (set.poses[f].accelMg[k] - s.correction.accelOffsetMg[k]) *
                      s.correction.accelScale[k];
      norm += c * c;
    }
    s.maxResidualMg = fmaxf(s.maxResidualMg, fabsf(sqrtf(norm) - kGravityMg));
  }
  *out = s;
  return true;
}

Record makeRecord(const Solution& solution) {
  Record r{};
  r.magic = kMagic;
  r.version = kVersion;
  r.size = sizeof(Record);
  r.correction = solution.correction;
  r.fullAxes = solution.fullAxes;
  r.faces = solution.faces;
  r.maxResidualMg = solution.maxResidualMg;
  r.gyroStdMdps = solution.gyroStdMdps;
  r.accelStdMg = solution.accelStdMg;
  r.samples = solution.samples;
  r.checksum = checksum(r);
  return r;
}

bool isValid(const Record& r) {
  return r.magic == kMagic && r.version == kVersion && r.size == sizeof(Record) &&
         r.checksum == checksum(r) && r.faces != 0 && (r.faces & ~0x3Fu) == 0 &&
         isPlausible(r.correction);
}

}  // namespace imu_sensor_calibration
}  // namespace core
