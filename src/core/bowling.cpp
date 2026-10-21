// Bowling logic: see include/core/bowling.h.
#include "core/bowling.h"

#include <math.h>
#include <string.h>

namespace core {
namespace bowling {

namespace {
constexpr float kPi = 3.14159265f;
constexpr float kDeg = kPi / 180.0f;

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float deadZone(float v, float dz) { return v > dz ? v - dz : (v < -dz ? v + dz : 0.0f); }
int bitCount(uint16_t m) {
  int n = 0;
  for (; m; m &= (uint16_t)(m - 1)) ++n;
  return n;
}

// ---- swing -> throw ----
constexpr float kSpeedBase = 4.0f, kSpeedPerDps = 0.02f,    // 2026-09-30: a relaxed 175 dps swing = 7.5 m/s
                 kSpeedMin = 3.0f, kSpeedMax = 11.0f;
constexpr float kHeadingDead = 3.0f, kHeadingGain = 0.08f, kHeadingMax = 2.5f;
constexpr float kTwistDead = 5.0f, kTwistFull = 23.0f;      // was 60, then 35: a moderate wrist turn hooks fully
// ---- swing reading ----
constexpr float kBackMinDeg = 20.0f;       // backswing needed before a forward swing counts (was 40, 28). A forward
                                           // lift then a drop looks exactly like a small backswing then a forward
                                           // swing to the gyro, so lifts up to ~18 deg are told apart, bigger ones not
constexpr float kForwardDps = 35.0f;       // swing rate reversed past this: the forward swing has started
constexpr float kAutoFraction = 0.35f;     // forward swing slowed to this share of its peak: auto release
constexpr float kAutoMinPeak = 70.0f;   // 2026-09-30: the release thresholds 30% lower too
constexpr float kReleaseMinPeak = 56.0f;
constexpr uint32_t kTapMs = 350;
constexpr float kTapDeg = 15.0f;
constexpr float kTiltMax = 0.4f;           // largest swing-plane tilt the twist correction trusts
// ---- lane ----
constexpr float kStep = 0.001f;
constexpr float kHookA = 3.0f;             // m/s^2 on the dry back end, full spin
constexpr float kHookCap = 0.55f;          // lateral m/s a full spin can add ("the ball has made its move")
constexpr float kOilFactor = 0.02f;        // hook force on the oil, relative to dry
constexpr float kRollDecel = 0.25f;        // m/s^2
constexpr float kBallMass = 6.8f, kPinMass = 1.5f;
constexpr float kBallPinE = 0.5f, kPinPinE = 0.5f;
constexpr float kFallSpeed = 0.5f;         // a standing pin pushed faster than this falls
constexpr float kDeckFriction = 3.5f;      // m/s^2 for sliding pins
constexpr float kPinReach = 0.30f;         // a tipping pin's head: this far along the fall direction when lying
constexpr float kHeadR = 0.035f;
constexpr float kLyingR = 0.07f;           // a lying pin is long: a slightly larger disc
constexpr float kGutterX = kLaneHalfW + kGutterW / 2;
constexpr float kSettleAfterHit = 4.0f, kMaxThrowS = 15.0f;
}  // namespace

void pinSpot(int i, float* x, float* y) {
  int row = 0;
  while ((row + 1) * (row + 2) / 2 <= i) ++row;
  const int j = i - row * (row + 1) / 2;
  *x = (j - row / 2.0f) * kPinSpacing;
  *y = kLaneLen + row * kRowSpacing;
}

bool isSplit(uint16_t st) {
  if (st & 1u) return false;                                  // head pin standing
  if (bitCount(st) < 2) return false;
  // Pins that touch in the triangle, or stand directly behind one another, count as one group.
  static const uint8_t kLinks[][2] = {{1, 2}, {1, 3}, {2, 3}, {2, 4}, {2, 5}, {3, 5}, {3, 6}, {4, 5}, {5, 6},
                                      {4, 7}, {4, 8}, {5, 8}, {5, 9}, {6, 9}, {6, 10}, {7, 8}, {8, 9}, {9, 10},
                                      {1, 5}, {2, 8}, {3, 9}};
  uint16_t group = (uint16_t)(st & -st);                      // start from the lowest standing pin
  for (bool grew = true; grew;) {
    grew = false;
    for (const auto& l : kLinks) {
      const uint16_t a = (uint16_t)(1u << (l[0] - 1)), b = (uint16_t)(1u << (l[1] - 1));
      if (!(st & a) || !(st & b)) continue;
      if ((group & a) && !(group & b)) { group |= b; grew = true; }
      if ((group & b) && !(group & a)) { group |= a; grew = true; }
    }
  }
  if (group != st) return true;
  // USBC: a pin down immediately ahead of and between two standing pins (4-5, 5-6, ...).
  static const uint8_t kAhead[][3] = {{2, 3, 1}, {4, 5, 2}, {5, 6, 3}, {7, 8, 4}, {8, 9, 5}, {9, 10, 6}};
  for (const auto& a : kAhead) {
    const uint16_t pair = (uint16_t)((1u << (a[0] - 1)) | (1u << (a[1] - 1)));
    if ((st & pair) == pair && !(st & (1u << (a[2] - 1)))) return true;
  }
  return false;
}

// ---------------- throw ----------------

float speedFor(float peakDps) { return clampf(kSpeedBase + peakDps * kSpeedPerDps, kSpeedMin, kSpeedMax); }

Throw makeThrow(const SwingReading& r, float aimDeg, float startX) {
  Throw t;
  t.speed = speedFor(r.peakDps);
  t.angleDeg = aimDeg + clampf(deadZone(r.headingDeg, kHeadingDead) * kHeadingGain, -kHeadingMax, kHeadingMax);
  t.spin = clampf(deadZone(r.twistDeg, kTwistDead) / kTwistFull, -1.0f, 1.0f);
  t.startX = startX;
  return t;
}

// ---------------- swing reading ----------------

SwingReader::SwingReader()
    : stage_(SwingStage::Idle), reading_{0, 0, 0, 0}, pressMs_(0), lastMs_(0), maxAngle_(0), maxSwing_(0), bp_(0),
      bu_(0), bf_(0), fp_(0), fu_(0), ff_(0), peak_(0) {}

void SwingReader::setBias(const float g[3]) { tr_.setBias(g); }

void SwingReader::press(uint32_t ms, const float acc[3]) {
  tr_.setUp(acc);                     // keeps the previous "up" if this reading is unclear
  tr_.begin();
  stage_ = SwingStage::Address;
  reading_ = SwingReading{0, 0, 0, 0};
  pressMs_ = lastMs_ = ms;
  maxAngle_ = maxSwing_ = 0;
  bp_ = bu_ = bf_ = fp_ = fu_ = ff_ = 0;
  peak_ = 0;
}

SwingResult SwingReader::sample(uint32_t ms, const float g[3]) {
  if (stage_ == SwingStage::Idle) return SwingResult::None;
  if ((int32_t)(ms - pressMs_) < 0) return SwingResult::None;   // from before the press
  lastMs_ = ms;
  // Every FIFO sample is 1/104 s after the one before. (Its timestamp is synthesised backwards from the poll time,
  // so differences across a batch boundary are 0..19 ms: integrating those would depend on the batching.)
  const float dt = 1.0f / 104.0f;
  tr_.update(g, dt);
  const float ang = tr_.angleDeg();
  if (ang > maxAngle_) maxAngle_ = ang;
  if (!tr_.planeKnown()) return SwingResult::None;
  const motion::Parts p = tr_.parts(), r = tr_.rates();
  if (stage_ == SwingStage::Address) stage_ = SwingStage::Backswing;
  if (stage_ == SwingStage::Backswing) {
    // The plane is learned from the first 20 degrees of motion. If that was a lift (forward/up) before the drop,
    // the swing then runs negative before any real backswing: turn the plane round and start the backswing over.
    if (maxSwing_ < kBackMinDeg && p.swing < -20.0f) {
      const motion::Vec3 a = tr_.plane();
      tr_.setPlane(motion::Vec3{-a.x, -a.y, -a.z});
      maxSwing_ = bp_ = bu_ = bf_ = 0;
      return SwingResult::None;
    }
    if (p.swing > maxSwing_) maxSwing_ = p.swing;
    if (maxSwing_ >= kBackMinDeg && r.swing < -kForwardDps) {
      stage_ = SwingStage::Forward;
    } else {
      bp_ += r.swing * dt;
      bu_ += r.yaw * dt;
      bf_ += r.twist * dt;
      return SwingResult::None;
    }
  }
  // Forward swing: rotation in the world frame. Plane part = the swing, forward-axis part = the swing plane turning
  // left/right (a swing across the body), vertical part = the wrist turning (the forearm points down at release).
  fp_ += r.swing * dt;
  fu_ += r.yaw * dt;
  ff_ += r.twist * dt;
  if (-r.swing > peak_) peak_ = -r.swing;
  if (peak_ >= kAutoMinPeak && -r.swing < kAutoFraction * peak_) return finish();
  return SwingResult::None;
}

SwingResult SwingReader::release(uint32_t ms) {
  if (stage_ == SwingStage::Idle) return SwingResult::None;
  if (stage_ == SwingStage::Forward && peak_ >= kReleaseMinPeak) return finish();
  const bool tap = (uint32_t)(ms - pressMs_) < kTapMs && maxAngle_ < kTapDeg;
  stage_ = SwingStage::Idle;
  return tap ? SwingResult::Tap : SwingResult::Cancel;
}

SwingResult SwingReader::finish() {
  reading_.peakDps = peak_;
  reading_.backswingDeg = maxSwing_;
  // Heading of the forward swing's axis in the horizontal plane (+ = right; fp_ is negative going forward),
  // relative to the backswing's own axis: the plane was learned from the first 20 degrees, which may include a
  // push-away drifting outward. Swinging through the same plane as the backswing is straight.
  float h = (atan2f(-ff_, -fp_) - (bp_ > 10.0f ? atan2f(bf_, bp_) : 0.0f)) / kDeg;
  if (h > 180) h -= 360;
  if (h < -180) h += 360;
  reading_.headingDeg = h;
  // A tilted swing plane puts some of the swing itself on the vertical axis: take out what the backswing showed.
  const float k = bp_ > 10.0f ? clampf(bu_ / bp_, -kTiltMax, kTiltMax) : 0.0f;
  reading_.twistDeg = -(fu_ - k * fp_);                       // counter-clockwise from above = left = negative
  stage_ = SwingStage::Idle;
  return SwingResult::Throw;
}

bool SwingReader::keepsChord(uint32_t ms) const {
  if (stage_ == SwingStage::Idle) return false;
  return stage_ != SwingStage::Address || (uint32_t)(ms - pressMs_) > kChordHoldMs;
}

float SwingReader::armDeg() const {
  if (stage_ == SwingStage::Idle) return 0;
  return tr_.planeKnown() ? tr_.parts().swing : tr_.angleDeg();
}

// ---------------- lane ----------------

Lane::Lane() : ball_(), pins_(), racked_(0), t_(0), firstHit_(-1) { rack(kFullRack); }

void Lane::rack(uint16_t standing) {
  racked_ = standing;
  for (int i = 0; i < kPins; ++i) {
    Pin& p = pins_[i];
    pinSpot(i, &p.x, &p.y);
    p.vx = p.vy = 0;
    p.tilt = 0;
    p.dir = 0;
    p.wobble = 0;
    p.out = false;
    p.state = (standing >> i) & 1 ? PinState::Standing : PinState::Gone;
  }
  ball_ = Ball{0, 0, 0, 0, 0, 0, false, false, false};
  t_ = 0;
  firstHit_ = -1;
}

void Lane::launch(const Throw& t) {
  const float a = t.angleDeg * kDeg;
  ball_ = Ball{t.startX, 0, t.speed * sinf(a), t.speed * cosf(a), t.spin * kHookCap, 0, false, false, true};
  t_ = 0;
  firstHit_ = -1;
}

void Lane::step(float dt) {
  // Whole 1 ms substeps; the remainder carries over so any frame time adds up exactly.
  static_assert(kStep > 0, "step");
  int n = (int)(dt / kStep + 0.5f);
  if (n < 1 && dt > 0) n = 1;
  if (n > 200) n = 200;                                        // a stalled frame must not freeze the UI
  for (int i = 0; i < n; ++i) substep();
}

void Lane::knock(Pin& p, float vx, float vy) {
  p.vx = vx;
  p.vy = vy;
  p.state = PinState::Falling;
  p.dir = atan2f(vy, vx);
  if (p.tilt < 3) p.tilt = 3;
}

static float pinRadius(const Pin& p) { return p.state == PinState::Standing ? kPinR : kLyingR; }
static bool pinMoving(const Pin& p) { return p.vx != 0 || p.vy != 0; }

// Impulse collision between two discs; returns true if they touched and were closing.
static bool collide(float& ax, float& ay, float& avx, float& avy, float ma, float& bx, float& by, float& bvx,
                    float& bvy, float mb, float dist, float e) {
  float nx = bx - ax, ny = by - ay;
  const float d = sqrtf(nx * nx + ny * ny);
  if (d >= dist || d < 1e-6f) return false;
  nx /= d;
  ny /= d;
  const float ia = 1.0f / ma, ib = 1.0f / mb;
  const float push = (dist - d) / (ia + ib);                  // separate by inverse mass
  ax -= nx * push * ia; ay -= ny * push * ia;
  bx += nx * push * ib; by += ny * push * ib;
  const float closing = (avx - bvx) * nx + (avy - bvy) * ny;
  if (closing <= 0) return false;
  const float j = (1 + e) * closing / (ia + ib);
  avx -= j * ia * nx; avy -= j * ia * ny;
  bvx += j * ib * nx; bvy += j * ib * ny;
  return true;
}

void Lane::substep() {
  t_ += kStep;
  Ball& b = ball_;
  if (b.live && !b.gone) {
    // Hook: friction turns the side rotation into lateral speed; little on the oil, fully on the dry back end.
    const float f = b.y < kOilEnd - 1 ? kOilFactor
                    : b.y > kOilEnd + 1 ? 1.0f
                                        : kOilFactor + (1 - kOilFactor) * (b.y - (kOilEnd - 1)) / 2;
    if (b.hook != 0 && !b.gutter) {
      float dv = (b.hook > 0 ? 1 : -1) * kHookA * f * kStep;
      if (fabsf(dv) > fabsf(b.hook)) dv = b.hook;
      b.vx += dv;
      b.hook -= dv;
    }
    const float v = sqrtf(b.vx * b.vx + b.vy * b.vy);
    if (v > 0.05f) {
      const float k = 1 - kRollDecel * kStep / v;
      b.vx *= k;
      b.vy *= k;
    }
    b.x += b.vx * kStep;
    b.y += b.vy * kStep;
    b.roll += v * kStep;
    if (!b.gutter && fabsf(b.x) > kLaneHalfW) {                // centre over the edge: it drops into the gutter
      b.gutter = true;
      b.x = b.x > 0 ? kGutterX : -kGutterX;
      b.vx = 0;
      b.hook = 0;
    }
    if (b.y > kPitY || v < 0.05f) b.gone = true;
  }
  // Pins: tip, slide, drop into the gutter or the pit, bounce off the kickbacks.
  for (Pin& p : pins_) {
    if (p.state == PinState::Gone) continue;
    if (p.wobble > 0) p.wobble = fmaxf(0, p.wobble - kStep / 0.8f);
    if (p.state == PinState::Falling) {
      p.tilt += (250 + 80 * sqrtf(p.vx * p.vx + p.vy * p.vy)) * kStep;
      if (p.tilt >= 90) { p.tilt = 90; p.state = PinState::Down; }
    }
    if (!pinMoving(p)) continue;
    p.x += p.vx * kStep;
    p.y += p.vy * kStep;
    const float v = sqrtf(p.vx * p.vx + p.vy * p.vy);
    const float k = v > kDeckFriction * kStep ? 1 - kDeckFriction * kStep / v : 0;
    p.vx *= k;
    p.vy *= k;
    if (v < 0.02f) p.vx = p.vy = 0;
    if (!p.out && fabsf(p.x) > kLaneHalfW + 0.01f && fabsf(p.vx) < 1.0f) {   // slow enough to drop in the gutter
      p.out = true;
      p.x = p.x > 0 ? kGutterX : -kGutterX;
      p.vx = 0;
      if (p.state == PinState::Standing) { p.state = PinState::Falling; p.dir = p.x > 0 ? 0 : kPi; }
    }
    if (fabsf(p.x) > kKickback - kPinR) {                      // flew over the gutter: off the kickback
      p.x = p.x > 0 ? kKickback - kPinR : -(kKickback - kPinR);
      p.vx = -p.vx * 0.5f;
    }
    if (p.y > kPitY) p.state = PinState::Gone;
  }
  // Ball into pins.
  if (b.live && !b.gone && !b.gutter && b.y > kLaneLen - 0.5f) {
    for (Pin& p : pins_) {
      if (p.state == PinState::Gone || p.out) continue;
      const bool standing = p.state == PinState::Standing;
      if (collide(b.x, b.y, b.vx, b.vy, kBallMass, p.x, p.y, p.vx, p.vy, kPinMass, kBallR + pinRadius(p), kBallPinE)) {
        if (firstHit_ < 0) firstHit_ = t_;
        if (standing) {
          if (sqrtf(p.vx * p.vx + p.vy * p.vy) > kFallSpeed) knock(p, p.vx, p.vy);
          else { p.vx = p.vy = 0; p.wobble = 1; }
        }
      }
    }
  }
  // Pins into pins.
  for (int i = 0; i < kPins; ++i) {
    Pin& a = pins_[i];
    if (a.state == PinState::Gone || a.out) continue;
    for (int j = i + 1; j < kPins; ++j) {
      Pin& c = pins_[j];
      if (c.state == PinState::Gone || c.out) continue;
      if (!pinMoving(a) && !pinMoving(c)) continue;
      const bool sa = a.state == PinState::Standing, sc = c.state == PinState::Standing;
      if (!collide(a.x, a.y, a.vx, a.vy, kPinMass, c.x, c.y, c.vx, c.vy, kPinMass, pinRadius(a) + pinRadius(c), kPinPinE))
        continue;
      if (firstHit_ < 0) firstHit_ = t_;
      if (sa) {
        if (sqrtf(a.vx * a.vx + a.vy * a.vy) > kFallSpeed) knock(a, a.vx, a.vy);
        else { a.vx = a.vy = 0; a.wobble = 1; }
      }
      if (sc) {
        if (sqrtf(c.vx * c.vx + c.vy * c.vy) > kFallSpeed) knock(c, c.vx, c.vy);
        else { c.vx = c.vy = 0; c.wobble = 1; }
      }
    }
  }
  // A tipping pin's head sweeps along its fall direction and takes down a standing pin it reaches.
  for (Pin& p : pins_) {
    if (p.state != PinState::Falling || p.tilt < 15 || p.out) continue;
    const float reach = kPinReach * sinf(p.tilt * kDeg);
    const float cx = cosf(p.dir), cy = sinf(p.dir);
    const float hx = p.x + cx * reach, hy = p.y + cy * reach;
    for (Pin& q : pins_) {
      if (&q == &p || q.state != PinState::Standing || q.out) continue;
      const float dx = q.x - hx, dy = q.y - hy;
      const float d = sqrtf(dx * dx + dy * dy);
      if (d >= kPinR + kHeadR) continue;
      float ux = dx / (d + 1e-6f) + cx, uy = dy / (d + 1e-6f) + cy;
      const float un = sqrtf(ux * ux + uy * uy) + 1e-6f;
      const float v = fmaxf(1.0f, 0.7f * sqrtf(p.vx * p.vx + p.vy * p.vy));
      knock(q, ux / un * v, uy / un * v);
    }
  }
}

bool Lane::settled() const {
  if (!ball_.live) return false;
  if (t_ > kMaxThrowS) return true;
  if (firstHit_ >= 0 && t_ - firstHit_ > kSettleAfterHit) return true;
  if (!ball_.gone) return false;
  for (const Pin& p : pins_) {
    if (p.state == PinState::Gone) continue;
    if (p.state == PinState::Falling || pinMoving(p) || p.wobble > 0) return false;
  }
  return true;
}

uint16_t Lane::standing() const {
  uint16_t m = 0;
  for (int i = 0; i < kPins; ++i)
    if (pins_[i].state == PinState::Standing) m |= (uint16_t)(1u << i);
  return m;
}

// ---------------- scoring ----------------

void Scorecard::reset() {
  memset(r_, 0, sizeof(r_));
  memset(split_, 0, sizeof(split_));
  n_ = 0;
}

int Scorecard::start(int f) const {
  int i = 0;
  for (int g = 0; g < f; ++g) {
    if (i >= n_) return -1;
    i += r_[i] == 10 ? 1 : 2;
  }
  return i <= n_ ? i : -1;
}

bool Scorecard::over() const {
  const int i = start(9);
  if (i < 0) return false;
  const int k = n_ - i;
  if (k >= 3) return true;
  return k == 2 && r_[i] + r_[i + 1] < 10;
}

bool Scorecard::roll(int pins, bool split) {
  if (over() || n_ >= 21) return false;
  r_[n_] = (uint8_t)(pins < 0 ? 0 : (pins > 10 ? 10 : pins));
  split_[n_] = split;
  ++n_;
  return true;
}

int Scorecard::frame() const {
  int i = 0;
  for (int f = 0; f < 9; ++f) {
    if (i >= n_) return f;                 // not started
    if (r_[i] == 10) { ++i; continue; }
    if (i + 1 >= n_) return f;             // one ball into an open frame
    i += 2;
  }
  return 9;
}

int Scorecard::ball() const {
  const int f = frame();
  const int s = start(f);
  return s < 0 ? 0 : n_ - s;
}

int Scorecard::total(int f) const {
  int sum = 0;
  for (int g = 0; g <= f; ++g) {
    const int i = start(g);
    if (i < 0 || i >= n_) return -1;
    int need, pts;
    if (g == 9) {
      const bool bonus = r_[i] == 10 || (i + 1 < n_ && r_[i] + r_[i + 1] == 10);
      need = bonus ? 3 : 2;
      if (i + need > n_) return -1;
      pts = r_[i] + r_[i + 1] + (bonus ? r_[i + 2] : 0);
    } else if (r_[i] == 10) {
      if (i + 3 > n_) return -1;
      pts = 10 + r_[i + 1] + r_[i + 2];
    } else {
      if (i + 2 > n_) return -1;
      if (r_[i] + r_[i + 1] == 10) {
        if (i + 3 > n_) return -1;
        pts = 10 + r_[i + 2];
      } else {
        pts = r_[i] + r_[i + 1];
      }
    }
    sum += pts;
  }
  return sum;
}

int Scorecard::score() const {
  int best = 0;
  for (int f = 0; f < 10; ++f) {
    const int t = total(f);
    if (t < 0) break;
    best = t;
  }
  return best;
}

static char digit(int n) { return n == 0 ? '-' : (n == 10 ? 'X' : (char)('0' + n)); }

void Scorecard::marks(int f, char out[4]) const {
  out[0] = 0;
  const int i = start(f);
  if (i < 0 || i >= n_) return;
  int k = 0;
  if (f < 9) {
    if (r_[i] == 10) { out[k++] = 'X'; }
    else {
      out[k++] = digit(r_[i]);
      if (i + 1 < n_) out[k++] = r_[i] + r_[i + 1] == 10 ? '/' : digit(r_[i + 1]);
    }
  } else {
    int rackLeft = 10;                                       // pins on the deck before each ball
    bool fresh = true;                                       // this ball is the first at a full rack
    for (int b = 0; b < 3 && i + b < n_; ++b) {
      const int v = r_[i + b];
      if (!fresh && v == rackLeft) out[k++] = '/';
      else out[k++] = digit(v);
      rackLeft -= v;
      fresh = rackLeft == 0;
      if (fresh) rackLeft = 10;
    }
  }
  out[k] = 0;
}

bool Scorecard::splitAt(int f, int b) const {
  const int i = start(f);
  return i >= 0 && i + b < n_ && split_[i + b];
}

// ---------------- match ----------------

void Match::reset() {
  card_.reset();
  rack_ = after_ = kFullRack;
  fresh_ = true;
  lane_.rack(rack_);
  phase_ = Phase::Aiming;
  outcome_ = Outcome::None;
  pos_ = 0;
  count_ = 0;
  aim_ = 0;
}

void Match::move(int boards) { setPosition(pos_ + boards); }

void Match::setPosition(int boards) {
  pos_ = boards < -kMaxBoards ? -kMaxBoards : (boards > kMaxBoards ? kMaxBoards : boards);
}

void Match::aim(int clicks) { setAim(aim_ + clicks * kAimStep); }

void Match::setAim(float deg) { aim_ = roundf(clampf(deg, -kMaxAim, kMaxAim) / kAimStep) * kAimStep; }

bool Match::throwBall(const Throw& t) {
  if (phase_ != Phase::Aiming) return false;
  lane_.rack(rack_);
  lane_.launch(t);
  phase_ = Phase::Rolling;
  return true;
}

bool Match::advance(float dtS) {
  if (phase_ != Phase::Rolling) return false;
  lane_.step(dtS);
  if (!lane_.settled()) return false;
  score(lane_.standing(), lane_.ball().gutter && !lane_.touched());
  return true;
}

void Match::score(uint16_t after, bool gutter) {
  if (phase_ != Phase::Rolling) return;
  after &= rack_;
  after_ = after;
  count_ = bitCount(rack_) - bitCount(after);
  const bool split = fresh_ && isSplit(after);
  card_.roll(count_, split);
  if (after == 0) outcome_ = fresh_ ? Outcome::Strike : Outcome::Spare;
  else if (split) outcome_ = Outcome::Split;
  else if (gutter && count_ == 0) outcome_ = Outcome::Gutter;
  else outcome_ = Outcome::Count;
  phase_ = Phase::Result;
}

void Match::next() {
  if (phase_ != Phase::Result) return;
  if (card_.over()) { phase_ = Phase::GameOver; return; }
  if (card_.ball() == 0 || after_ == 0) {
    rack_ = kFullRack;
    fresh_ = true;
  } else {
    rack_ = after_;
    fresh_ = false;
  }
  lane_.rack(rack_);
  phase_ = Phase::Aiming;
}

}  // namespace bowling
}  // namespace core
