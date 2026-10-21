#pragma once

#include <stdint.h>

namespace core {
namespace imu_calibration {

struct Vec3 {
  float x;
  float y;
  float z;
};

struct Profile {
  uint32_t magic;
  uint16_t version;
  uint16_t size;
  float gyroBias[3];
  float neutralAccel[3];
  float axes[3][3];  // Rows: application roll, pitch, yaw in sensor coordinates.
  float quality[3];
  uint32_t checksum;
};

struct Orientation {
  float w;
  float x;
  float y;
  float z;
};

Vec3 add(Vec3 a, Vec3 b);
Vec3 scale(Vec3 v, float amount);
float dot(Vec3 a, Vec3 b);
Vec3 cross(Vec3 a, Vec3 b);
float length(Vec3 v);
Vec3 normalize(Vec3 v);

bool buildProfile(const Vec3& neutralAccel, const Vec3& gyroBias,
                  const Vec3 measuredAxes[3], const float quality[3],
                  Profile* out);
bool isValid(const Profile& profile);
bool load(Profile* out);
bool save(const Profile& profile);
bool clear();

Vec3 transform(const Profile& profile, Vec3 sensorVector);
Vec3 rotate(const Orientation& orientation, Vec3 vector);

class Tracker {
 public:
  Tracker();
  void reset(const Profile& profile);
  void update(const Profile& profile, Vec3 accelMg, Vec3 gyroMdps, float dtSeconds);
  Orientation orientation() const { return q_; }
  Vec3 ratesDps() const { return ratesDps_; }
  /** Quaternion logarithm expressed as signed degrees about the calibrated
   * roll, pitch, and yaw axes. Unlike Euler angles, this has no 90-degree
   * pitch singularity. */
  Vec3 rotationVectorDegrees() const;
  Vec3 eulerDegrees() const;

 private:
  Orientation q_;
  Vec3 ratesDps_;
  Vec3 neutralGravity_;
};

}  // namespace imu_calibration
}  // namespace core
