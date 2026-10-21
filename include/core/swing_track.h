#pragma once
/**
 * Swing tracking for motion games (bowling, golf): integrates the gyro into an orientation from a neutral pose
 * and splits the rotation into the three parts a swing is judged by, whatever the grip:
 *   - swing: about the swing-plane axis (horizontal, learned from the first 20 degrees of the backswing);
 *   - yaw:   about the true vertical ("up", from the accelerometer at the neutral pose) -> direction;
 *   - twist: about the third axis (forward = plane x up) -> spin / face angle.
 * Pure C++ (no Arduino), host-tested in test/test_swing_track.
 *
 * Frames: everything is expressed in the neutral device frame (the device's axes at begin()).
 * Units: degrees and degrees/second. Gyro samples are device-frame dps, bias-corrected by the caller or setBias().
 */
#include <stdint.h>

namespace core {
namespace motion {

struct Vec3 {
  float x, y, z;
};

struct Parts {
  float swing, yaw, twist;
};

class SwingTracker {
 public:
  SwingTracker();
  void setBias(const float gyroDps[3]);      // subtracted from every sample
  bool setUp(const float accMg[3]);          // gravity reaction at rest ("up" in the device frame); false if unclear
  void begin();                              // the current pose becomes neutral; forget the plane
  void update(const float gyroDps[3], float dtS);

  float angleDeg() const;                    // total rotation from neutral
  Vec3 rotationDeg() const;                  // axis * angle, neutral frame
  Vec3 rateDps() const;                      // latest angular velocity, neutral frame
  Vec3 bodyRateDps() const { return body_; } // latest angular velocity in the device's own frame (bias removed)
  float lastDt() const { return dt_; }       // the time step of the latest update (s)
  Vec3 toBody(const Vec3& v) const;         // a neutral-frame direction in the device's current frame
  bool planeKnown() const { return planeKnown_; }
  Vec3 plane() const { return plane_; }
  Vec3 up() const { return up_; }
  Parts parts() const;                       // rotationDeg split along plane / up / forward (0 until the plane is known)
  Parts rates() const;                       // rateDps split the same way
  // Force the swing-plane axis (e.g. a game that knows the swing goes back about the device's own x axis).
  void setPlane(const Vec3& axis);

 private:
  Parts split(const Vec3& v) const;
  float qw_, qx_, qy_, qz_;                  // orientation: neutral -> current
  float bias_[3];
  Vec3 up_, plane_, forward_, rate_, body_;
  float dt_;
  bool planeKnown_;
};

}  // namespace motion
}  // namespace core
