#include "core/imu_calibration.h"

#include <Preferences.h>
#include <math.h>
#include <string.h>

namespace core {
namespace imu_calibration {

namespace {

static constexpr uint32_t kProfileMagic = 0x494D5543u;  // IMUC
static constexpr uint16_t kProfileVersion = 1;
static constexpr float kPi = 3.14159265358979323846f;

uint32_t checksum(const Profile& source) {
  Profile profile = source;
  profile.checksum = 0;
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(&profile);
  uint32_t hash = 2166136261u;
  for (size_t i = 0; i < sizeof(profile); ++i) {
    hash ^= bytes[i];
    hash *= 16777619u;
  }
  return hash;
}

Orientation normalizeQuaternion(Orientation q) {
  const float magnitude = sqrtf(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
  if (magnitude < 0.000001f) return {1.0f, 0.0f, 0.0f, 0.0f};
  const float inverse = 1.0f / magnitude;
  return {q.w * inverse, q.x * inverse, q.y * inverse, q.z * inverse};
}

Orientation multiply(Orientation a, Orientation b) {
  return {
      a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
      a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
      a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
  };
}

Vec3 rotateInverse(Orientation q, Vec3 vector) {
  return rotate({q.w, -q.x, -q.y, -q.z}, vector);
}

}  // namespace

Vec3 add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 scale(Vec3 v, float amount) { return {v.x * amount, v.y * amount, v.z * amount}; }
float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(Vec3 a, Vec3 b) {
  return {a.y * b.z - a.z * b.y,
          a.z * b.x - a.x * b.z,
          a.x * b.y - a.y * b.x};
}
float length(Vec3 v) { return sqrtf(dot(v, v)); }
Vec3 normalize(Vec3 v) {
  const float magnitude = length(v);
  return magnitude < 0.000001f ? Vec3{0.0f, 0.0f, 0.0f} : scale(v, 1.0f / magnitude);
}

bool buildProfile(const Vec3& neutralAccel, const Vec3& gyroBias,
                  const Vec3 measuredAxes[3], const float quality[3], Profile* out) {
  if (!out) return false;
  Vec3 roll = normalize(measuredAxes[0]);
  Vec3 pitch = add(measuredAxes[1], scale(roll, -dot(measuredAxes[1], roll)));
  pitch = normalize(pitch);
  if (length(roll) < 0.9f || length(pitch) < 0.9f) return false;

  Vec3 yaw = normalize(cross(roll, pitch));
  if (dot(yaw, measuredAxes[2]) < 0.0f) yaw = scale(yaw, -1.0f);
  pitch = normalize(cross(yaw, roll));
  if (dot(pitch, measuredAxes[1]) < 0.0f) {
    pitch = scale(pitch, -1.0f);
    yaw = scale(yaw, -1.0f);
  }

  Profile profile{};
  profile.magic = kProfileMagic;
  profile.version = kProfileVersion;
  profile.size = sizeof(Profile);
  const Vec3 neutral = normalize(neutralAccel);
  const Vec3 axes[3] = {roll, pitch, yaw};
  profile.gyroBias[0] = gyroBias.x;
  profile.gyroBias[1] = gyroBias.y;
  profile.gyroBias[2] = gyroBias.z;
  profile.neutralAccel[0] = neutral.x;
  profile.neutralAccel[1] = neutral.y;
  profile.neutralAccel[2] = neutral.z;
  for (int row = 0; row < 3; ++row) {
    profile.axes[row][0] = axes[row].x;
    profile.axes[row][1] = axes[row].y;
    profile.axes[row][2] = axes[row].z;
    profile.quality[row] = quality[row];
  }
  profile.checksum = checksum(profile);
  *out = profile;
  return true;
}

bool isValid(const Profile& profile) {
  if (profile.magic != kProfileMagic || profile.version != kProfileVersion ||
      profile.size != sizeof(Profile) || profile.checksum != checksum(profile)) return false;
  for (int row = 0; row < 3; ++row) {
    const Vec3 axis{profile.axes[row][0], profile.axes[row][1], profile.axes[row][2]};
    if (length(axis) < 0.98f || length(axis) > 1.02f) return false;
  }
  const Vec3 r{profile.axes[0][0], profile.axes[0][1], profile.axes[0][2]};
  const Vec3 p{profile.axes[1][0], profile.axes[1][1], profile.axes[1][2]};
  const Vec3 y{profile.axes[2][0], profile.axes[2][1], profile.axes[2][2]};
  return fabsf(dot(r, p)) < 0.03f && fabsf(dot(r, y)) < 0.03f &&
         fabsf(dot(p, y)) < 0.03f;
}

bool load(Profile* out) {
  if (!out) return false;
  Preferences preferences;
  // Open read/write so Preferences creates the namespace on first use instead
  // of emitting a misleading NVS NOT_FOUND error for an uncalibrated device.
  if (!preferences.begin("imu_cal", false)) return false;
  const size_t stored = preferences.getBytesLength("profile");
  const size_t read = stored == sizeof(Profile)
                          ? preferences.getBytes("profile", out, sizeof(Profile)) : 0;
  preferences.end();
  return read == sizeof(Profile) && isValid(*out);
}

bool save(const Profile& source) {
  if (!isValid(source)) return false;
  Preferences preferences;
  if (!preferences.begin("imu_cal", false)) return false;
  const size_t written = preferences.putBytes("profile", &source, sizeof(Profile));
  preferences.end();
  return written == sizeof(Profile);
}

bool clear() {
  Preferences preferences;
  if (!preferences.begin("imu_cal", false)) return false;
  const bool removed = preferences.remove("profile");
  preferences.end();
  return removed;
}

Vec3 transform(const Profile& profile, Vec3 sensorVector) {
  return {
      profile.axes[0][0] * sensorVector.x + profile.axes[0][1] * sensorVector.y +
          profile.axes[0][2] * sensorVector.z,
      profile.axes[1][0] * sensorVector.x + profile.axes[1][1] * sensorVector.y +
          profile.axes[1][2] * sensorVector.z,
      profile.axes[2][0] * sensorVector.x + profile.axes[2][1] * sensorVector.y +
          profile.axes[2][2] * sensorVector.z,
  };
}

Vec3 rotate(const Orientation& q, Vec3 vector) {
  const Orientation v{0.0f, vector.x, vector.y, vector.z};
  const Orientation inverse{q.w, -q.x, -q.y, -q.z};
  const Orientation result = multiply(multiply(q, v), inverse);
  return {result.x, result.y, result.z};
}

Tracker::Tracker() { reset(Profile{}); }

void Tracker::reset(const Profile& profile) {
  q_ = {1.0f, 0.0f, 0.0f, 0.0f};
  ratesDps_ = {0.0f, 0.0f, 0.0f};
  if (isValid(profile)) {
    neutralGravity_ = normalize(transform(
        profile, {profile.neutralAccel[0], profile.neutralAccel[1], profile.neutralAccel[2]}));
  } else {
    neutralGravity_ = {0.0f, 0.0f, 1.0f};
  }
}

void Tracker::update(const Profile& profile, Vec3 accelMg, Vec3 gyroMdps, float dtSeconds) {
  if (!isValid(profile) || dtSeconds <= 0.0f || dtSeconds > 0.1f) return;
  const Vec3 bias{profile.gyroBias[0], profile.gyroBias[1], profile.gyroBias[2]};
  ratesDps_ = scale(transform(profile, add(gyroMdps, scale(bias, -1.0f))), 0.001f);

  Vec3 omega = scale(ratesDps_, kPi / 180.0f);
  const Vec3 measuredGravity = normalize(transform(profile, accelMg));
  const float accelMagnitude = length(accelMg);
  if (accelMagnitude > 850.0f && accelMagnitude < 1150.0f) {
    const Vec3 expectedGravity = normalize(rotateInverse(q_, neutralGravity_));
    const Vec3 correction = cross(measuredGravity, expectedGravity);
    omega = add(omega, scale(correction, 1.8f));
  }

  const Orientation derivative = multiply(q_, {0.0f, omega.x, omega.y, omega.z});
  q_.w += 0.5f * derivative.w * dtSeconds;
  q_.x += 0.5f * derivative.x * dtSeconds;
  q_.y += 0.5f * derivative.y * dtSeconds;
  q_.z += 0.5f * derivative.z * dtSeconds;
  q_ = normalizeQuaternion(q_);
}

Vec3 Tracker::rotationVectorDegrees() const {
  // q and -q describe the same orientation. Select the representation with a
  // non-negative scalar component so the reported rotation is the shortest
  // signed path in [-180, 180] degrees and remains continuous through the
  // Euler pitch singularity at 90 degrees.
  Orientation q = q_;
  if (q.w < 0.0f) {
    q.w = -q.w;
    q.x = -q.x;
    q.y = -q.y;
    q.z = -q.z;
  }
  const float vectorMagnitude = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z);
  if (vectorMagnitude < 0.000001f) return {0.0f, 0.0f, 0.0f};
  const float angleDegrees = 2.0f * atan2f(vectorMagnitude, q.w) * 180.0f / kPi;
  const float amount = angleDegrees / vectorMagnitude;
  return {q.x * amount, q.y * amount, q.z * amount};
}

Vec3 Tracker::eulerDegrees() const {
  const float sinRoll = 2.0f * (q_.w * q_.x + q_.y * q_.z);
  const float cosRoll = 1.0f - 2.0f * (q_.x * q_.x + q_.y * q_.y);
  const float sinPitch = 2.0f * (q_.w * q_.y - q_.z * q_.x);
  const float sinYaw = 2.0f * (q_.w * q_.z + q_.x * q_.y);
  const float cosYaw = 1.0f - 2.0f * (q_.y * q_.y + q_.z * q_.z);
  return {
      atan2f(sinRoll, cosRoll) * 180.0f / kPi,
      asinf(fmaxf(-1.0f, fminf(1.0f, sinPitch))) * 180.0f / kPi,
      atan2f(sinYaw, cosYaw) * 180.0f / kPi,
  };
}

}  // namespace imu_calibration
}  // namespace core
