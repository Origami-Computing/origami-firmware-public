// Golf game logic: see include/core/golf.h.
#include "core/golf.h"

#include <math.h>
#include <string.h>

namespace core {
namespace golf {

namespace {
constexpr float kDeg = 3.14159265f / 180.0f;
float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
}  // namespace

// ---------------- clubs ----------------

namespace {
const Club kClubs[kClubCount] = {
    {"Driver", "1W", 230, 11, 0.0038f, 1.00f, true},
    {"3-wood", "3W", 210, 13, 0.0036f, 0.88f, true},
    {"5-iron", "5I", 180, 17, 0.0034f, 0.70f, false},
    {"7-iron", "7I", 155, 21, 0.0032f, 0.56f, false},
    {"9-iron", "9I", 130, 27, 0.0030f, 0.42f, false},
    {"Pitching wedge", "PW", 110, 32, 0.0028f, 0.32f, false},
    {"Sand wedge", "SW", 80, 38, 0.0026f, 0.22f, false},
    {"Putter", "PT", 25, 0, 0, 1.0f, false},
};
}  // namespace

const Club& club(int i) { return kClubs[i < 0 ? 0 : (i >= kClubCount ? kClubCount - 1 : i)]; }

float lieFactor(Surface lie, int c) {
  if (c == kPutter) {
    switch (lie) {
      case Surface::Green: case Surface::Fringe: case Surface::Fairway: case Surface::Tee: return 1.0f;
      case Surface::Rough: return 0.6f;
      case Surface::Waste: return 0.7f;
      case Surface::Bunker: return 0.25f;
      default: return 1.0f;
    }
  }
  const bool wood = club(c).wood;
  switch (lie) {
    case Surface::Rough: return wood ? 0.72f : 0.80f;
    case Surface::Waste: return wood ? 0.78f : 0.85f;
    case Surface::Bunker: return c == kSand ? 0.88f : 0.45f;
    default: return 1.0f;
  }
}

// ---------------- swing ----------------

SwingTuning defaultTuning() {
  SwingTuning t;
  // A hand-held kami: a relaxed flick is ~200 dps, a firm swing 500-600, a wild one 800+ (tennis measured ~600).
  t.armDps = 90; t.putterArmDps = 15;
  t.countFrac = 0.2f; t.putterCountDps = 25;    // driver 120 dps .. sand wedge 96 (short chips stay possible)
  const float full[kClubCount] = {600, 580, 560, 540, 520, 500, 480, 180};   // driver .. sand wedge, putter
  for (int i = 0; i < kClubCount; ++i) t.fullDps[i] = full[i];
  t.overFactor = 1.35f; t.putterOverFactor = 2.5f;
  t.impactFrac = 0.5f; t.impactHoldMs = 400;
  t.yawGain = 0.6f; t.putterYawGain = 0.3f;
  t.twistGain = 0.6f; t.putterTwistGain = 0.15f;
  t.perfectYaw = 3; t.perfectTwist = 5;
  t.yawSign = 1; t.twistSign = 1;
  t.overLong = 0.04f; t.overDriftDeg = 5; t.overCurveDeg = 4;
  t.perfectBonus = 0.03f;
  return t;
}

void SwingAnalyzer::begin(int club, const SwingTuning& tuning) {
  t_ = tuning;
  putter_ = club == kPutter;
  full_ = tuning.fullDps[club < 0 ? 0 : (club >= kClubCount ? kClubCount - 1 : club)];
  phase_ = SwingPhase::Address;
  peak_ = 0;
  startMs_ = peakMs_ = 0;
  havePrev_ = false;
  memset(&r_, 0, sizeof(r_));
}

float SwingAnalyzer::countDps() const { return putter_ ? t_.putterCountDps : t_.countFrac * full_; }

float SwingAnalyzer::meterFill() const {
  if (phase_ == SwingPhase::Impact) return r_.meterPower;
  if (phase_ == SwingPhase::Address) return 0;
  return peak_ / full_;
}

bool SwingAnalyzer::meterRed() const {
  if (phase_ == SwingPhase::Impact) return r_.grade == SwingGrade::Over;
  return peak_ > full_ * (putter_ ? t_.putterOverFactor : t_.overFactor);
}

void SwingAnalyzer::liveShape(float* startDeg, float* curveDeg) const {
  if (phase_ == SwingPhase::Impact) { *startDeg = r_.startDeg; *curveDeg = r_.curveDeg; return; }
  const float yaw = havePrev_ ? prev_.yaw * t_.yawSign : 0, twist = havePrev_ ? prev_.twist * t_.twistSign : 0;
  *startDeg = clampf(yaw * (putter_ ? t_.putterYawGain : t_.yawGain), -20, 20);
  *curveDeg = clampf(twist * (putter_ ? t_.putterTwistGain : t_.twistGain), -20, 20);
}

void SwingAnalyzer::liveFacePath(float* face, float* path) const {
  if (phase_ == SwingPhase::Impact) { *face = r_.twistDeg; *path = r_.yawDeg; return; }
  *face = havePrev_ ? prev_.twist * t_.twistSign : 0;
  *path = havePrev_ ? prev_.yaw * t_.yawSign : 0;
}

SwingPhase SwingAnalyzer::feed(const SwingSample& s) {
  if (phase_ == SwingPhase::Impact) return phase_;
  prev_ = s;
  havePrev_ = true;
  const float r = fabsf(s.swingRate);
  switch (phase_) {
    case SwingPhase::Address:
      if (r >= (putter_ ? t_.putterArmDps : t_.armDps)) {    // a forward swing starts (either way)
        phase_ = SwingPhase::Downswing;
        peak_ = r;
        startMs_ = peakMs_ = s.ms;
      }
      break;
    case SwingPhase::Downswing:
      if (r > peak_) { peak_ = r; peakMs_ = s.ms; }
      if (r < t_.impactFrac * peak_ || (int32_t)(s.ms - peakMs_) > (int32_t)t_.impactHoldMs) {
        if (peak_ >= countDps()) {
          finish(s);
          phase_ = SwingPhase::Impact;
        } else {                                              // too gentle: aiming or drift, not a stroke
          phase_ = SwingPhase::Address;
          peak_ = 0;
        }
      }
      break;
    default:
      break;
  }
  return phase_;
}

void SwingAnalyzer::finish(const SwingSample& s) {
  SwingResult& r = r_;
  r.topDeg = 0;
  r.impactDps = peak_;
  r.neededDps = full_;
  r.speed = peak_ / full_;
  r.power = clampf(r.speed, 0, 1);
  r.meterPower = r.speed;
  r.downMs = (uint16_t)clampf((float)(int32_t)(s.ms - startMs_), 0, 60000);
  r.yawDeg = s.yaw * t_.yawSign;
  r.twistDeg = s.twist * t_.twistSign;
  const bool over = r.speed > (putter_ ? t_.putterOverFactor : t_.overFactor);
  r.grade = over ? SwingGrade::Over : SwingGrade::Good;
  r.overswing = over;
  r.tooSlow = false;
  r.perfect = !over && fabsf(r.yawDeg) <= t_.perfectYaw && fabsf(r.twistDeg) <= t_.perfectTwist;
  r.quality = 1.0f;
  r.startDeg = r.yawDeg * (putter_ ? t_.putterYawGain : t_.yawGain);
  r.curveDeg = r.twistDeg * (putter_ ? t_.putterTwistGain : t_.twistGain);
  r.driftSide = 0;
  if (over) {
    // past the line: a little longer and out of control, to the side the face points (or either way if square)
    const float side = r.twistDeg > 1.5f ? 1.0f : (r.twistDeg < -1.5f ? -1.0f : (((s.ms / 10) & 1) ? 1.0f : -1.0f));
    const float k = putter_ ? 0.3f : 1.0f;
    r.quality = 1.0f + t_.overLong;
    r.driftSide = side;
    r.startDeg += side * t_.overDriftDeg * k;
    r.curveDeg += side * t_.overCurveDeg * k;
  }
  if (r.perfect) {
    r.quality *= 1.0f + t_.perfectBonus;
    r.startDeg = 0;
    r.curveDeg = 0;
  }
  r.startDeg = clampf(r.startDeg, -20, 20);
  r.curveDeg = clampf(r.curveDeg, -20, 20);
  // a forward stroke has no backswing to measure a follow-through against: no backspin (the flight model keeps it)
  r.followDeg = 0;
  r.backspin = 0;
  r.followDone = true;
}

SwingFeedback swingFeedback(const SwingAnalyzer& a) {
  SwingFeedback f;
  memset(&f, 0, sizeof(f));
  const SwingPhase ph = a.phase();
  if (ph == SwingPhase::Address) return f;
  f.active = true;
  f.frozen = ph == SwingPhase::Impact;
  f.phase = f.frozen ? 3 : 2;
  f.fill = a.meterFill();
  f.target = 0;
  f.red = a.meterRed();
  f.tempo = f.fill;
  a.liveFacePath(&f.faceDeg, &f.pathDeg);
  f.followDone = f.frozen;
  f.grade = f.frozen ? a.result().grade : (f.red ? SwingGrade::Over : SwingGrade::Good);
  return f;
}

// ---------------- reader ----------------

namespace {
using core::motion::Vec3;
float dot3(const Vec3& a, const Vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Vec3 cross3(const Vec3& a, const Vec3& b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
Vec3 unit3(const Vec3& a) {
  const float n = sqrtf(dot3(a, a));
  return n > 1e-6f ? Vec3{a.x / n, a.y / n, a.z / n} : Vec3{0, 0, 0};
}
}  // namespace

void SwingReader::begin() {
  haveAxis_ = false;
  haveLast_ = false;
  lastRaw_ = offset_ = 0;
  yaw_ = twist_ = 0;
}

SwingSample SwingReader::unwrap(SwingSample s) {
  const float raw = s.swing;
  if (haveLast_) {
    const float step = raw - lastRaw_;
    if (step < -180.0f) offset_ += 360.0f;                  // passed +180: the tracker now says about -180
    else if (step > 180.0f) offset_ -= 360.0f;              // and back
  }
  lastRaw_ = raw;
  haveLast_ = true;
  s.swing = raw + offset_;
  return s;
}

SwingSample SwingReader::read(const core::motion::SwingTracker& t, uint32_t ms, bool allowFlip) {
  SwingSample s;
  s.ms = ms;
  const Vec3 up = t.up();
  if (!haveAxis_) {
    // The grip: the kami held up, screen toward you. The IMU's z axis is the screen normal (flat face-up reads
    // gravity on z); which in-plane axis points up depends on the mounting, so the axes come from the screen normal
    // and gravity only: the backswing raises the face toward you (a turn about the horizontal axis across the screen,
    // normal x up), tilting the kami right/left turns it about the screen normal (the curve), and the path is a
    // turn about the true vertical. Not learned from the first motion (an aiming tilt used to become "the swing").
    const Vec3 n{0, 0, 1};
    Vec3 nh{n.x - up.x * dot3(n, up), n.y - up.y * dot3(n, up), n.z - up.z * dot3(n, up)};
    nh = unit3(nh);
    if (nh.x == 0 && nh.y == 0 && nh.z == 0) nh = Vec3{1, 0, 0};   // screen facing the sky: no better guess
    axis_ = unit3(cross3(nh, up));
    fwd_ = unit3(cross3(axis_, up));
    haveAxis_ = true;
  }
  const Vec3 rot = t.rotationDeg(), rate = t.rateDps();
  if (allowFlip && dot3(rot, axis_) < -4.0f) {          // the first real move went the other way: turn round
    axis_ = Vec3{-axis_.x, -axis_.y, -axis_.z};
    fwd_ = unit3(cross3(axis_, up));
    haveLast_ = false;                                    // a new direction: start unwrapping afresh
    offset_ = 0;
    yaw_ = twist_ = 0;
  }
  {  // split this sample's body rate into swing (axis_), tilt (fwd_) and path (the current vertical, in the kami's
     // frame): omega = a*axis + b*fwd + c*vertical. The axis and fwd are fixed in the kami (the swing turns about
     // the axis); the vertical moves as the kami pitches, which is what used to mix a path into the tilt.
    const Vec3 w = t.bodyRateDps(), u = t.toBody(up);
    const float dt = t.lastDt();
    const Vec3 fxu = cross3(fwd_, u), uxa = cross3(u, axis_), axf = cross3(axis_, fwd_);
    const float det = dot3(axis_, fxu);
    float b, c;
    if (fabsf(det) > 0.3f) {
      b = dot3(w, uxa) / det;
      c = dot3(w, axf) / det;
    } else {                                              // pitched ~90 deg: the path can't be told from the tilt
      b = dot3(w, fwd_);
      c = 0;
    }
    twist_ += b * dt;
    yaw_ += c * dt;
  }
  s.swing = dot3(rot, axis_);
  s.yaw = yaw_;
  s.twist = twist_;
  s.swingRate = dot3(rate, axis_);
  return unwrap(s);
}

// ---------------- synthetic swings ----------------

SwingResult synthesizeSwing(float peakDps, float yawDeg, float twistDeg, int club, const SwingTuning& tuning) {
  SwingAnalyzer a;
  a.begin(club, tuning);
  uint32_t ms = 1000;
  for (int i = 0; i < 10; ++i) a.feed({ms += 10, 0, 0, 0, 0});           // address
  const float D = 0.25f, dt = 1.0f / 104.0f;
  float sw = 0;
  for (int i = 1; i * dt <= D + 0.2f; ++i) {
    const float t = i * dt;
    const float u = t / D;
    const float rate = u <= 1 ? peakDps * sinf(3.14159265f * u) * sinf(3.14159265f * u) : 0;
    const float k = clampf(2.0f * u, 0, 1);                                // path and tilt built up by the peak
    sw -= rate * dt;
    if (a.feed({ms + (uint32_t)(t * 1000), sw, yawDeg * k, twistDeg * k, -rate}) == SwingPhase::Impact) break;
  }
  return a.result();
}

// ---------------- flight ----------------

namespace {
constexpr float kG = 10.73f;          // yd/s^2
constexpr float kDrag = 0.0042f;      // per yard
constexpr float kSpinDecayS = 6.0f;
constexpr float kDt = 0.01f;

struct SurfaceFeel {
  float restitution, keep, decel;
};
SurfaceFeel feel(Surface s) {
  switch (s) {
    case Surface::Tee: return {0.25f, 0.50f, 2.8f};
    case Surface::Fairway: return {0.25f, 0.50f, 2.8f};
    case Surface::Fringe: return {0.22f, 0.40f, 1.6f};
    case Surface::Green: return {0.22f, 0.35f, 0.65f};
    case Surface::Rough: return {0.15f, 0.35f, 4.5f};
    case Surface::Waste: return {0.20f, 0.40f, 3.4f};
    case Surface::Bunker: return {0.0f, 0.05f, 14.0f};
    default: return {0.0f, 0.0f, 50.0f};
  }
}

struct Air {
  float x, y, h, vx, vy, vz, lift, side, t;
};

// One flight step. wx, wy: wind (yd/s) at 10 m; it is weaker near the ground.
inline void airStep(Air& a, float wx, float wy) {
  const float wf = 0.75f + 0.5f * fminf(1.0f, fmaxf(0.0f, a.h) / 25.0f);
  const float rx = a.vx - wx * wf, ry = a.vy - wy * wf, rz = a.vz;
  const float sp = sqrtf(rx * rx + ry * ry + rz * rz);
  const float hs = sqrtf(rx * rx + ry * ry) + 1e-6f;
  const float hx = rx / hs, hy = ry / hs;
  float ax = -kDrag * sp * rx, ay = -kDrag * sp * ry, az = -kG - kDrag * sp * rz;
  ax += a.lift * sp * (-rz * hx);
  ay += a.lift * sp * (-rz * hy);
  az += a.lift * sp * hs;
  ax += a.side * sp * sp * hy;                               // + side = to the right of travel
  ay += a.side * sp * sp * (-hx);
  a.vx += ax * kDt; a.vy += ay * kDt; a.vz += az * kDt;
  a.x += a.vx * kDt; a.y += a.vy * kDt; a.h += a.vz * kDt;
  a.t += kDt;
  const float decay = 1.0f - kDt / kSpinDecayS;
  a.lift *= decay;
  a.side *= decay;
}

// Calm-air carry (along +y) and sideways drift for a launch speed.
void calmCarry(float v0, float launchDeg, float lift, float side, float* carry, float* lateral) {
  Air a = {0, 0, 0, 0, v0 * cosf(launchDeg * kDeg), v0 * sinf(launchDeg * kDeg), lift, side, 0};
  while (a.t < 20.0f) {
    airStep(a, 0, 0);
    if (a.h < 0 && a.vz < 0) break;
  }
  *carry = a.y;
  *lateral = a.x;
}

float solveSpeed(float carry, float launchDeg, float lift) {
  float lo = 1, hi = 160;
  for (int i = 0; i < 22; ++i) {
    const float mid = 0.5f * (lo + hi);
    float c, l;
    calmCarry(mid, launchDeg, lift, 0, &c, &l);
    if (c < carry) lo = mid; else hi = mid;
  }
  return 0.5f * (lo + hi);
}

struct Recorder {
  Point3* out;
  int max, n;
  float every, next;
  void add(float t, float x, float y, float h) {
    if (!out || t + 1e-4f < next) return;
    if (n < max) out[n++] = {x, y, h};
    next += every;
  }
  void last(float x, float y, float h) {
    if (!out || max <= 0) return;
    if (n < max) out[n++] = {x, y, h};
    else out[max - 1] = {x, y, h};
  }
};

// Distance from p to the segment a-b.
float segDist(float ax, float ay, float bx, float by, float px, float py) {
  const float dx = bx - ax, dy = by - ay;
  const float l2 = dx * dx + dy * dy;
  float t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
  t = t < 0 ? 0 : (t > 1 ? 1 : t);
  const float qx = ax + dx * t - px, qy = ay + dy * t - py;
  return sqrtf(qx * qx + qy * qy);
}

Surface quickSurface(const HoleMap& map, float x, float y) {
  if (map.onGreen(x, y)) return Surface::Green;
  return map.surfaceAt(x, y);
}

// Roll on the ground from (x, y) with velocity (vx, vy). Updates the result; returns true when holed.
void roll(const HoleMap& map, float x, float y, float vx, float vy, float t, Recorder& rec, ShotResult& r,
          float* lastDryX, float* lastDryY) {
  const float px = map.pinX(), py = map.pinY();
  float sp = sqrtf(vx * vx + vy * vy);
  int steps = 0;
  while (steps++ < 3000) {
    const Surface s = quickSurface(map, x, y);
    if (isPenalty(s)) {
      r.outcome = s == Surface::OutOfBounds ? Outcome::OutOfBounds : Outcome::Water;
      r.x = x; r.y = y;
      rec.last(x, y, 0);
      return;
    }
    *lastDryX = x; *lastDryY = y;
    const SurfaceFeel f = feel(s);
    float ax = 0, ay = 0;
    const bool sloped = s == Surface::Green || s == Surface::Fringe;
    if (sloped) { ax = map.slopeX(); ay = map.slopeY(); }
    if (sp < 0.03f) {
      const float slope = sqrtf(ax * ax + ay * ay);
      if (slope < f.decel * 0.5f) break;                    // friction holds it
    }
    const float ux = sp > 1e-4f ? vx / sp : 0, uy = sp > 1e-4f ? vy / sp : 0;
    vx += (ax - ux * f.decel) * kDt;
    vy += (ay - uy * f.decel) * kDt;
    const float nsp = sqrtf(vx * vx + vy * vy);
    if (vx * ux + vy * uy < 0 && nsp < f.decel * kDt * 2) { vx = vy = 0; }   // friction stops it, no reversal
    const float nx = x + vx * kDt, ny = y + vy * kDt;
    if (segDist(x, y, nx, ny, px, py) < kCupRadius && sp < 1.6f) {
      r.outcome = Outcome::Holed;
      r.x = px; r.y = py;
      rec.add(t, px, py, 0);
      rec.last(px, py, -0.1f);
      return;
    }
    x = nx; y = ny;
    sp = sqrtf(vx * vx + vy * vy);
    t += kDt;
    rec.add(t, x, y, 0);
    if (sp < 0.01f && !sloped) break;
  }
  r.outcome = Outcome::Rest;
  r.x = x; r.y = y;
  rec.last(x, y, 0);
}

float groundDist(float x0, float y0, float x1, float y1) { return sqrtf((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0)); }

}  // namespace

float headingTo(float x0, float y0, float x1, float y1) { return atan2f(x1 - x0, y1 - y0) / kDeg; }

float putterRangeFor(float dist) {
  float r = ceilf(dist * 1.25f / 5.0f) * 5.0f;
  return clampf(r, 5, 30);
}

ShotResult playShot(const HoleMap& map, const Wind& wind, const ShotInput& in, Point3* traj, int trajMax, float trajDt) {
  ShotResult r;
  memset(&r, 0, sizeof(r));
  r.outcome = Outcome::Rest;
  Recorder rec = {traj, trajMax, 0, trajDt, 0};
  const Club& c = club(in.club);
  const float lf = lieFactor(in.lie, in.club);
  const float heading = (in.aimDeg + in.swing.startDeg) * kDeg;
  const float dx = sinf(heading), dy = cosf(heading);
  float lastDryX = in.x, lastDryY = in.y;
  rec.add(0, in.x, in.y, 0);

  if (in.club == kPutter) {
    const float dist = fmaxf(0.0f, in.putterRange * in.swing.power * in.swing.quality * lf);
    // The range is for a green; grass slows the ball more, so the stroke pays for the grass before the green.
    const float dGreen = feel(Surface::Green).decel;
    float v2 = 2.0f * dGreen * dist;
    for (float u = 0.125f; u < dist; u += 0.25f) {
      const Surface ps = quickSurface(map, in.x + dx * u, in.y + dy * u);
      if (ps == Surface::Green || isPenalty(ps)) break;
      v2 += 2.0f * (feel(ps).decel - dGreen) * 0.25f;
    }
    const float v = sqrtf(v2);
    const float h2 = heading + in.swing.curveDeg * kDeg;       // putter: the face sends it off line
    r.landX = in.x; r.landY = in.y;
    roll(map, in.x, in.y, v * sinf(h2), v * cosf(h2), 0, rec, r, &lastDryX, &lastDryY);
  } else {
    const float carry = c.carry * in.swing.power * in.swing.quality * lf;
    const float v0 = carry > 0.5f ? solveSpeed(carry, c.launchDeg, c.lift) : 0.5f;
    float side = 0;
    if (fabsf(in.swing.curveDeg) > 0.05f && carry > 1) {
      float cc, lat;
      const float probe = 1e-4f;
      calmCarry(v0, c.launchDeg, c.lift, probe, &cc, &lat);
      if (fabsf(lat) > 1e-3f) side = probe * (cc * tanf(in.swing.curveDeg * kDeg)) / lat;
    }
    const float ws = wind.mph * kMphToYps;
    const float wx = ws * sinf(wind.dirDeg * kDeg), wy = ws * cosf(wind.dirDeg * kDeg);
    // launch: heading in the x/y plane; side force is relative to travel
    Air a = {in.x, in.y, 0, v0 * cosf(c.launchDeg * kDeg) * dx, v0 * cosf(c.launchDeg * kDeg) * dy,
             v0 * sinf(c.launchDeg * kDeg), c.lift, side, 0};
    uint8_t hitMask[HoleMap::kMaxTrees / 8] = {};
    for (int i = 0; i < map.treeCount(); ++i) {           // a ball under a tree can be punched out from under it
      const Tree& tr = map.tree(i);
      if ((in.x - tr.x) * (in.x - tr.x) + (in.y - tr.y) * (in.y - tr.y) <= tr.r * tr.r) hitMask[i >> 3] |= (uint8_t)(1 << (i & 7));
    }
    bool landed = false;
    int bounces = 0;
    int step = 0;
    while (a.t < 25.0f) {
      airStep(a, wx, wy);
      if (a.h > r.apex) r.apex = a.h;
      rec.add(a.t, a.x, a.y, fmaxf(a.h, 0.0f));
      // trees: below the canopy top and inside it -> knocked down
      for (int i = 0; i < map.treeCount(); ++i) {
        const Tree& tr = map.tree(i);
        if (a.h >= tr.h || (hitMask[i >> 3] & (1 << (i & 7)))) continue;
        const float ddx = a.x - tr.x, ddy = a.y - tr.y;
        if (ddx * ddx + ddy * ddy > tr.r * tr.r) continue;
        hitMask[i >> 3] |= (uint8_t)(1 << (i & 7));
        r.hitTree = true;
        const float k = (tr.variant & 1) ? 0.25f : -0.25f;
        const float vx = a.vx, vy = a.vy;
        a.vx = -0.12f * vx + k * vy * 0.3f;
        a.vy = -0.12f * vy - k * vx * 0.3f;
        a.vz = fminf(a.vz, 0.0f) * 0.3f;
        a.lift = 0;
        a.side = 0;
      }
      if ((++step % 4) == 0 && a.h > 0) {
        const Surface s = quickSurface(map, a.x, a.y);
        if (!isPenalty(s)) { lastDryX = a.x; lastDryY = a.y; }
      }
      if (a.h > 0 || a.vz >= 0) continue;
      // touchdown
      a.h = 0;
      if (!landed) {
        landed = true;
        r.landX = a.x; r.landY = a.y;
        r.carry = groundDist(in.x, in.y, a.x, a.y);
        r.curveYd = (a.x - in.x) * cosf(in.aimDeg * kDeg) - (a.y - in.y) * sinf(in.aimDeg * kDeg);
        r.flightS = a.t;
        if (groundDist(a.x, a.y, map.pinX(), map.pinY()) < kCupRadius) {         // straight in
          r.outcome = Outcome::Holed;
          r.x = map.pinX(); r.y = map.pinY();
          rec.last(r.x, r.y, -0.1f);
          break;
        }
      }
      const Surface s = quickSurface(map, a.x, a.y);
      if (isPenalty(s)) {
        r.outcome = s == Surface::OutOfBounds ? Outcome::OutOfBounds : Outcome::Water;
        r.x = a.x; r.y = a.y;
        rec.last(a.x, a.y, 0);
        break;
      }
      lastDryX = a.x; lastDryY = a.y;
      const SurfaceFeel f = feel(s);
      float keep = f.keep * (bounces == 0 ? c.rollKeep : 1.0f);
      if (bounces == 0 && in.swing.backspin > 0) {           // backspin checks it up (and back, on a green)
        const float bs = in.swing.backspin;
        keep *= 1.0f - 0.8f * bs;
        if (s == Surface::Green || s == Surface::Fringe) keep -= 0.12f * bs;
      }
      a.vx *= keep;
      a.vy *= keep;
      a.vz = -a.vz * f.restitution;
      a.lift = 0;
      a.side = 0;
      ++bounces;
      if (a.vz < 1.5f || s == Surface::Bunker) {
        roll(map, a.x, a.y, a.vx, a.vy, a.t, rec, r, &lastDryX, &lastDryY);
        break;
      }
    }
    if (!landed) { r.x = a.x; r.y = a.y; }
  }

  if (r.outcome == Outcome::Water) {
    // drop on the last dry ground, 2 yards back toward where the shot came from
    const float bx = in.x - lastDryX, by = in.y - lastDryY;
    const float bl = sqrtf(bx * bx + by * by);
    float x = lastDryX, y = lastDryY;
    if (bl > 2.5f) { x += bx / bl * 2.0f; y += by / bl * 2.0f; }
    const Surface s = map.surfaceAt(x, y);
    if (isPenalty(s)) { x = in.x; y = in.y; }
    r.x = x; r.y = y;
  } else if (r.outcome == Outcome::OutOfBounds) {
    r.x = in.x; r.y = in.y;                                   // stroke and distance
  }
  r.lie = r.outcome == Outcome::OutOfBounds ? in.lie : (r.outcome == Outcome::Holed ? Surface::Green : map.surfaceAt(r.x, r.y));
  if (r.outcome == Outcome::Water && r.x == in.x && r.y == in.y) r.lie = in.lie;
  r.total = groundDist(in.x, in.y, r.outcome == Outcome::Rest ? r.x : r.landX, r.outcome == Outcome::Rest ? r.y : r.landY);
  if (r.outcome == Outcome::Holed) r.total = groundDist(in.x, in.y, map.pinX(), map.pinY());
  if (r.outcome == Outcome::Rest && groundDist(r.x, r.y, map.pinX(), map.pinY()) <= kGimme &&
      (r.lie == Surface::Green || r.lie == Surface::Fringe))
    r.gimme = true;
  if (!r.carry && in.club == kPutter) r.carry = r.total;
  r.points = rec.n;
  return r;
}

// ---------------- live preview ----------------

int previewShot(const Wind& wind, int ci, Surface lie, float x, float y, float aimDeg, const SwingResult& sw,
                float putterRange, Point3* out) {
  const float lf = lieFactor(lie, ci);
  if (ci == kPutter) {
    const float dist = fmaxf(0.0f, putterRange * sw.power * sw.quality * lf);
    const float h = (aimDeg + sw.startDeg + sw.curveDeg) * kDeg;
    for (int i = 0; i < kPreviewPoints; ++i) {
      const float u = dist * i / (kPreviewPoints - 1);
      out[i] = {x + sinf(h) * u, y + cosf(h) * u, 0};
    }
    return kPreviewPoints;
  }
  const Club& c = club(ci);
  const float carry = c.carry * sw.power * sw.quality * lf;
  const float h = (aimDeg + sw.startDeg) * kDeg;
  const float dx = sinf(h), dy = cosf(h);                    // along the start line
  const float nx = cosf(h), ny = -sinf(h);                   // to its right
  // wind, as the flight model feels it (fitted to it): carry change and sideways drift, growing through the flight
  const float ws = wind.mph * kMphToYps;
  const float along = ws * cosf((wind.dirDeg * kDeg) - h), cross = ws * sinf((wind.dirDeg * kDeg) - h);
  const float carryW = carry * (1.0f + (along >= 0 ? 0.0085f : 0.0125f) * along);
  const float drift = 0.0122f * carry * cross;
  const float bend = carry * tanf(sw.curveDeg * kDeg);       // sidespin: the calm landing is carry x tan(curve)
  for (int i = 0; i < kPreviewPoints; ++i) {
    const float u = (float)i / (kPreviewPoints - 1);
    const float a = carryW * u, side = (bend + drift) * u * u;
    const float hgt = 4.0f * (10.0f + 0.07f * carry) * u * (1.0f - u);
    out[i] = {x + dx * a + nx * side, y + dy * a + ny * side, hgt};
  }
  return kPreviewPoints;
}

MeterBend meterBend(float startDeg, float curveDeg, float fill, bool over, float driftSide) {
  MeterBend m;
  // curve bends it most (the face), the start direction (path) a little; 10 degrees is a full bend
  const float b = (curveDeg + 0.4f * startDeg) / 10.0f;
  m.bend = b > 1 ? 1 : (b < -1 ? -1 : b);
  m.overshoot = over ? clampf(fill - 1.0f, 0.08f, 0.35f) : 0.0f;
  m.overSide = over ? (driftSide != 0 ? (driftSide > 0 ? 1.0f : -1.0f) : (m.bend >= 0 ? 1.0f : -1.0f)) : 0.0f;
  return m;
}

float smoothToward(float current, float target, float dtMs, float tauMs) {
  if (tauMs <= 0 || dtMs <= 0) return target;
  return current + (target - current) * (1.0f - expf(-dtMs / tauMs));
}

const char* shotShape(const SwingResult& r) {
  if (r.grade == SwingGrade::Over) return "Over-swing";
  const float s = r.startDeg, c = r.curveDeg;
  if (fabsf(s) < 1.5f && fabsf(c) < 1.5f) return "Straight";
  if (fabsf(c) >= fabsf(s)) {
    if (c > 0) return c > 5 ? "Slice" : "Fade";
    return c < -5 ? "Hook" : "Draw";
  }
  return s > 0 ? "Push" : "Pull";
}

// ---------------- caddie ----------------

namespace {
const float kRollEst[kClubCount] = {20, 16, 10, 7, 4, 3, 2, 0};

float strokesFrom(float d, Surface s) {
  if (s == Surface::Green) return 1.0f + 0.95f * (1.0f - expf(-d / 5.0f));
  float f = d <= 150 ? 2.0f + d / 150.0f : 3.0f + (d - 150.0f) / 220.0f;
  if (d < 30) f = 2.0f + d / 100.0f;
  switch (s) {
    case Surface::Rough: return f + 0.25f;
    case Surface::Bunker: return f + 0.45f;
    case Surface::Waste: return f + 0.2f;
    default: return f;
  }
}

float rollFor(int c, Surface s, float power) {
  float k = 1.0f;
  switch (s) {
    case Surface::Green: k = 0.8f; break;
    case Surface::Fringe: k = 0.8f; break;
    case Surface::Rough: k = 0.25f; break;
    case Surface::Waste: k = 0.4f; break;
    case Surface::Bunker: k = 0.0f; break;
    default: break;
  }
  return kRollEst[c] * k * power;
}

// Does the flight from (x, y) to the landing point clip a tree? apex ~ 12 + 0.06 carry yards.
bool treeInPath(const HoleMap& map, float x, float y, float lx, float ly, float carry) {
  const float dx = lx - x, dy = ly - y;
  const float l2 = dx * dx + dy * dy;
  if (l2 < 1) return false;
  const float apex = 10.0f + 0.07f * carry;
  for (int i = 0; i < map.treeCount(); ++i) {
    const Tree& t = map.tree(i);
    const float u = ((t.x - x) * dx + (t.y - y) * dy) / l2;
    if (u < 0 || u > 1.05f) continue;
    const float cx = x + dx * u - t.x, cy = y + dy * u - t.y;
    if (cx * cx + cy * cy > (t.r + 1.0f) * (t.r + 1.0f)) continue;
    const float h = 4.0f * apex * u * (1.0f - u);
    if (h < t.h + 1.0f) return true;
  }
  return false;
}

// Expected strokes after hitting club c at power p toward heading hd (calm-air equivalent carry `carry`).
float evaluate(const HoleMap& map, float x, float y, int c, float carry, float hdDeg, float power, Surface lie) {
  static const float kAim[3] = {-3.5f, 0.0f, 3.5f};
  static const float kLen[3] = {-0.07f, 0.0f, 0.05f};
  float sum = 0;
  const float px = map.pinX(), py = map.pinY();
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      const float hd = (hdDeg + kAim[i]) * kDeg;
      const float cr = carry * (1.0f + kLen[j]);
      const float lx = x + sinf(hd) * cr, ly = y + cosf(hd) * cr;
      Surface s = map.surfaceAt(lx, ly);
      float cost;
      if (s == Surface::OutOfBounds) {
        cost = 1.0f + strokesFrom(groundDist(x, y, px, py), lie) + 0.3f;
      } else if (s == Surface::Water || s == Surface::Canyon) {
        cost = 1.0f + strokesFrom(groundDist(lx, ly, px, py) + 15.0f, Surface::Rough) + 0.3f;
      } else {
        const float rl = rollFor(c, s, power);
        const float rx = lx + sinf(hd) * rl, ry = ly + cosf(hd) * rl;
        const Surface rs = rl > 0.5f ? map.surfaceAt(rx, ry) : s;
        if (isPenalty(rs)) cost = 1.0f + strokesFrom(groundDist(rx, ry, px, py) + 10.0f, Surface::Rough) + 0.2f;
        else cost = strokesFrom(groundDist(rx, ry, px, py), rs);
      }
      sum += cost;
    }
  return sum / 9.0f;
}

Plan putt(const HoleMap& map, float x, float y, Surface lie, bool readGreen) {
  Plan p;
  const float d = groundDist(x, y, map.pinX(), map.pinY());
  p.club = kPutter;
  p.putterRange = putterRangeFor(d);
  p.aimDeg = headingTo(x, y, map.pinX(), map.pinY());
  const float lf = lieFactor(lie, kPutter);
  p.power = clampf((d + 0.35f) / (p.putterRange * lf), 0.05f, 1.0f);
  p.targetX = map.pinX();
  p.targetY = map.pinY();
  if (!readGreen) return p;
  // read the break: search aim and pace with the real roll
  SwingResult sw;
  memset(&sw, 0, sizeof(sw));
  sw.quality = 1;
  ShotInput in = {x, y, lie, kPutter, 0, p.putterRange, sw};
  const Wind calm = {0, 0};
  float best = 1e9f, bestAim = p.aimDeg, bestPow = p.power;
  auto tryShot = [&](float aim, float pw) {
    in.aimDeg = aim;
    in.swing.power = clampf(pw, 0.02f, 1.0f);
    const ShotResult r = playShot(map, calm, in, nullptr, 0);
    float score = r.outcome == Outcome::Holed ? -1.0f + fabsf(pw - p.power) * 0.1f
                                              : groundDist(r.x, r.y, map.pinX(), map.pinY());
    if (r.outcome != Outcome::Holed && r.outcome != Outcome::Rest) score += 100;
    if (score < best) { best = score; bestAim = aim; bestPow = in.swing.power; }
  };
  const float span = clampf(40.0f / (d + 1.0f), 2.0f, 12.0f);
  for (float da = -span; da <= span + 0.01f; da += span / 8.0f)
    for (float k = 0.85f; k <= 1.31f; k += 0.075f) tryShot(p.aimDeg + da, p.power * k);
  const float a0 = bestAim, p0 = bestPow;
  for (float da = -span / 8.0f; da <= span / 8.0f + 0.001f; da += span / 32.0f)
    for (float k = 0.95f; k <= 1.051f; k += 0.025f) tryShot(a0 + da, p0 * k);
  p.aimDeg = bestAim;
  p.power = bestPow;
  return p;
}
}  // namespace

Plan caddie(const HoleMap& map, const Wind& wind, float x, float y, Surface lie, bool readGreen) {
  const float px = map.pinX(), py = map.pinY();
  const float d = groundDist(x, y, px, py);
  if (lie == Surface::Green || ((lie == Surface::Fringe || lie == Surface::Tee) && d < 25) ||
      (lie == Surface::Fairway && d < 15))
    return putt(map, x, y, lie, readGreen);

  const float ws = wind.mph * kMphToYps;
  Plan best;
  memset(&best, 0, sizeof(best));
  best.club = kSand;
  best.aimDeg = headingTo(x, y, px, py);
  best.power = 0.5f;
  best.targetX = px; best.targetY = py;
  float bestCost = 1e9f;

  auto consider = [&](float tx, float ty, bool isPin) {
    const float dist = groundDist(x, y, tx, ty);
    if (dist < 8) return;
    const float hd = headingTo(x, y, tx, ty);
    const float along = ws * cosf((wind.dirDeg - hd) * kDeg);   // + helping
    const float cross = ws * sinf((wind.dirDeg - hd) * kDeg);   // + pushes right
    const Surface ts = map.surfaceAt(tx, ty);
    for (int c = 0; c < kPutter; ++c) {
      if (c == kDriver && lie != Surface::Tee) continue;
      const float lf = lieFactor(lie, c);
      const float full = club(c).carry * lf;
      // the carry that leaves the ball at the target after the roll, then the calm carry that flies that far
      const float roll = rollFor(c, isPin ? Surface::Green : ts, 1.0f);
      float need = dist - roll * fminf(1.0f, dist / full);
      const float windK = along >= 0 ? 0.008f : 0.011f;
      const float calm = need / (1.0f + windK * along);
      const float power = calm / full;
      const float minPow = (c >= kPitch) ? 0.22f : 0.45f;       // a forward stroke's floor is 20 % power
      if (power < minPow || power > 1.0f) continue;
      const float drift = 0.011f * calm * cross;
      const float aim = hd - atanf(drift / fmaxf(dist, 1.0f)) / kDeg;
      // a line through a tree goes nowhere: the stroke is wasted (then everything still to play from here)
      const float carryHere = dist - roll * fminf(1.0f, dist / full);
      const float lx = x + sinf(hd * kDeg) * carryHere, ly = y + cosf(hd * kDeg) * carryHere;
      float cost = treeInPath(map, x, y, lx, ly, carryHere)
                       ? strokesFrom(groundDist(x, y, px, py), lie) + 0.5f
                       : evaluate(map, x, y, c, carryHere, hd, power, lie);
      cost += 1.0f + 0.25f * (1.0f - power) + (c == kDriver ? 0.04f : 0.0f);   // full swings are easier to repeat
      if (cost < bestCost) {
        bestCost = cost;
        best.club = c;
        best.aimDeg = aim;
        best.power = power;
        best.targetX = tx;
        best.targetY = ty;
      }
    }
  };
  consider(px, py, true);
  // escape: back to the nearest point of the route (and a little beyond), e.g. chipping out sideways from the trees
  for (int i = 0; i + 1 < map.routePoints(); ++i) {
    float ax, ay, bx, by;
    map.routePoint(i, &ax, &ay);
    map.routePoint(i + 1, &bx, &by);
    const float dx = bx - ax, dy = by - ay, l2 = dx * dx + dy * dy;
    if (l2 < 1) continue;
    const float len = sqrtf(l2);
    const float u = clampf(((x - ax) * dx + (y - ay) * dy) / l2, 0, 1);
    for (float ahead = 0; ahead <= 30.0f; ahead += 15.0f) {
      const float uu = clampf(u + ahead / len, 0, 1);
      consider(ax + dx * uu, ay + dy * uu, false);
    }
  }
  // lay-up targets along the route, from where the ball is
  for (int i = 0; i + 1 < map.routePoints(); ++i) {
    float ax, ay, bx, by;
    map.routePoint(i, &ax, &ay);
    map.routePoint(i + 1, &bx, &by);
    const float len = groundDist(ax, ay, bx, by);
    const float nx = -(by - ay) / len, ny = (bx - ax) / len;
    for (float u = 0; u < len; u += 10.0f) {
      const float cx = ax + (bx - ax) * u / len, cy = ay + (by - ay) * u / len;
      if (groundDist(x, y, cx, cy) > 270.0f || cy < y - 5.0f) continue;
      for (int k = -1; k <= 1; ++k) consider(cx + nx * 9.0f * k, cy + ny * 9.0f * k, false);
    }
  }
  best.putterRange = putterRangeFor(d);
  return best;
}

// ---------------- scoring ----------------

const char* scoreName(int strokes, int par) {
  if (strokes == 1) return "Hole in one!";
  switch (strokes - par) {
    case -3: return "Albatross!";
    case -2: return "Eagle!";
    case -1: return "Birdie!";
    case 0: return "Par";
    case 1: return "Bogey";
    case 2: return "Double bogey";
    case 3: return "Triple bogey";
    default: return strokes < par ? "Amazing!" : "Keep going";
  }
}

}  // namespace golf
}  // namespace core
