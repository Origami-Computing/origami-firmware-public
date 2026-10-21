// Swing tracking: see include/core/swing_track.h.
#include "core/swing_track.h"

#include <math.h>

namespace core {
namespace motion {

namespace {
constexpr float kDeg = 3.14159265f / 180.0f;
constexpr float kPlaneLearnDeg = 20.0f;

float dot(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 scale(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
Vec3 sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float norm(const Vec3& a) { return sqrtf(dot(a, a)); }
Vec3 unit(const Vec3& a) {
  const float n = norm(a);
  return n > 1e-9f ? scale(a, 1.0f / n) : Vec3{0, 0, 0};
}
}  // namespace

SwingTracker::SwingTracker()
    : qw_(1), qx_(0), qy_(0), qz_(0), bias_{0, 0, 0}, up_{0, 0, 1}, plane_{1, 0, 0}, forward_{0, 1, 0},
      rate_{0, 0, 0}, body_{0, 0, 0}, dt_(0), planeKnown_(false) {}

void SwingTracker::setBias(const float g[3]) {
  for (int i = 0; i < 3; ++i) bias_[i] = g[i];
}

bool SwingTracker::setUp(const float a[3]) {
  const Vec3 v{a[0], a[1], a[2]};
  if (norm(v) < 500.0f) return false;                     // not at rest / no clear gravity (mg)
  up_ = unit(v);
  return true;
}

void SwingTracker::begin() {
  qw_ = 1;
  qx_ = qy_ = qz_ = 0;
  rate_ = {0, 0, 0};
  planeKnown_ = false;
}

void SwingTracker::setPlane(const Vec3& axis) {
  Vec3 h = sub(axis, scale(up_, dot(axis, up_)));         // keep it horizontal
  if (norm(h) < 1e-6f) return;
  plane_ = unit(h);
  forward_ = unit(cross(plane_, up_));
  planeKnown_ = true;
}

void SwingTracker::update(const float g[3], float dt) {
  const float wx = (g[0] - bias_[0]) * kDeg, wy = (g[1] - bias_[1]) * kDeg, wz = (g[2] - bias_[2]) * kDeg;
  body_ = {wx / kDeg, wy / kDeg, wz / kDeg};
  dt_ = dt;
  // q <- q * dq, dq = rotation by w*dt about the body axis (exact for a constant rate over dt).
  const float wn = sqrtf(wx * wx + wy * wy + wz * wz);
  const float half = 0.5f * wn * dt;
  const float s = wn > 1e-9f ? sinf(half) / wn : 0.5f * dt;
  const float dw = cosf(half), dx = wx * s, dy = wy * s, dz = wz * s;
  const float nw = qw_ * dw - qx_ * dx - qy_ * dy - qz_ * dz;
  const float nx = qw_ * dx + qx_ * dw + qy_ * dz - qz_ * dy;
  const float ny = qw_ * dy - qx_ * dz + qy_ * dw + qz_ * dx;
  const float nz = qw_ * dz + qx_ * dy - qy_ * dx + qz_ * dw;
  const float n = sqrtf(nw * nw + nx * nx + ny * ny + nz * nz);
  qw_ = nw / n; qx_ = nx / n; qy_ = ny / n; qz_ = nz / n;
  // Body rate expressed in the neutral frame: v' = q v q*.
  const Vec3 u{qx_, qy_, qz_}, v{wx / kDeg, wy / kDeg, wz / kDeg};
  const Vec3 t = scale(cross(u, v), 2.0f);
  rate_ = {v.x + qw_ * t.x + cross(u, t).x, v.y + qw_ * t.y + cross(u, t).y, v.z + qw_ * t.z + cross(u, t).z};
  if (!planeKnown_ && angleDeg() >= kPlaneLearnDeg) setPlane(rotationDeg());
}

float SwingTracker::angleDeg() const {
  const float w = qw_ < 0 ? -qw_ : qw_;
  return 2.0f * acosf(w > 1.0f ? 1.0f : w) / kDeg;
}

Vec3 SwingTracker::rotationDeg() const {
  const float sgn = qw_ < 0 ? -1.0f : 1.0f;                // shortest rotation
  const Vec3 axis = unit(Vec3{qx_ * sgn, qy_ * sgn, qz_ * sgn});
  return scale(axis, angleDeg());
}

Vec3 SwingTracker::rateDps() const { return rate_; }

Vec3 SwingTracker::toBody(const Vec3& v) const {
  // body = q* v q (the inverse of the body -> neutral rotation used for rate_)
  const Vec3 u{-qx_, -qy_, -qz_};
  const Vec3 t = scale(cross(u, v), 2.0f);
  const Vec3 c = cross(u, t);
  return {v.x + qw_ * t.x + c.x, v.y + qw_ * t.y + c.y, v.z + qw_ * t.z + c.z};
}

Parts SwingTracker::split(const Vec3& v) const {
  if (!planeKnown_) return {0, 0, 0};
  return {dot(v, plane_), dot(v, up_), dot(v, forward_)};
}

Parts SwingTracker::parts() const { return split(rotationDeg()); }
Parts SwingTracker::rates() const { return split(rate_); }

}  // namespace motion
}  // namespace core
