#pragma once
/**
 * Golf: pure game logic for a Wii-Sports-Resort-style golf game on the kami.
 *
 *  - Clubs (carry, launch, spin, roll) and lies.
 *  - SwingReader: turns core::motion::SwingTracker into signed swing / yaw / twist, also for putts smaller than the
 *    tracker's 20 degree plane-learning threshold.
 *  - SwingAnalyzer: address -> backswing (top = power) -> downswing -> impact (passing back through neutral), and
 *    the grade: speed at impact vs. the backswing, yaw (push/pull), twist (face: slice/hook), perfect, overswing.
 *  - Ball flight: drag, lift, sidespin and wind; bounce and roll per surface; trees, water, canyon, out of bounds,
 *    sloped greens and the cup.
 *  - Caddie: club, aim and power for a sensible target (used to pre-select the club, by `golf auto` and the sims).
 *  - Scoring names.
 * Units: yards, seconds, degrees. Headings are degrees clockwise from +y (see golf_course.h).
 * No Arduino, no allocation: host-tested in test/test_golf/test_host.cpp.
 */
#include <stdint.h>

#include "core/golf_course.h"
#include "core/swing_track.h"

namespace core {
namespace golf {

// ---------------- clubs ----------------

constexpr int kClubCount = 8;
constexpr int kDriver = 0, kWood3 = 1, kIron5 = 2, kIron7 = 3, kIron9 = 4, kPitch = 5, kSand = 6, kPutter = 7;

struct Club {
  const char* name;
  const char* shortName;
  float carry;        // yards at 100 % in calm air from a good lie
  float launchDeg;
  float lift;         // backspin lift coefficient
  float rollKeep;     // share of the horizontal speed kept at the first bounce (backspin checks it up)
  bool wood;
};
const Club& club(int index);

// Distance factor of a lie for a club (1 = clean).
float lieFactor(Surface lie, int clubIndex);

// ---------------- swing ----------------
// 2026-09-30, the user: "get rid of the whole backswing mechanic ... just be like Wii Sports (the original) with the
// forward swing, and your swing direction makes it go left or right." A stroke is one forward swing: hold B (that
// pose is neutral), then swing - a fast turn about the swing axis, either way. Power is how hard (the peak rate);
// the path (yaw) and the tilt (twist) steer it.

struct SwingTuning {
  float armDps, putterArmDps;          // a forward swing starts when the swing rate passes this
  float countFrac, putterCountDps;     // and is a stroke only if its peak reaches countFrac x full (putter: this)
                                       // (slower: aiming or drift); so the weakest stroke is countFrac power
  float fullDps[kClubCount];           // the peak rate for full power, per club (the putter's is its range)
  float overFactor, putterOverFactor;  // past fullDps x this: an over-swing
  float impactFrac;                    // impact when the rate has dropped to this share of its peak...
  uint16_t impactHoldMs;               // ...or this long after the peak
  float yawGain, putterYawGain;        // degrees of start direction per degree of path (yaw) at impact
  float twistGain, putterTwistGain;    // degrees of curve per degree of tilt (twist) at impact
  float perfectYaw, perfectTwist;      // a perfect stroke: this square (and not an over-swing)
  float yawSign, twistSign;            // flip if the device shows them backwards
  float overLong, overDriftDeg, overCurveDeg;   // an over-swing: a little long, drifting to one side
  float perfectBonus;
};
SwingTuning defaultTuning();

struct SwingSample {
  uint32_t ms;
  float swing, yaw, twist;             // degrees from address
  float swingRate;                     // dps about the swing axis (either sign)
};

// Address (B held, waiting), Downswing (the forward swing is under way), Impact. Backswing is not used any more.
enum class SwingPhase : uint8_t { Address, Backswing, Downswing, Impact };
enum class SwingGrade : uint8_t { Weak, Good, Over };   // Over = past the line (red)

struct SwingResult {
  float topDeg;          // unused (no backswing); kept for logs
  float power;           // peak / full, capped at 1
  float impactDps;       // the forward swing's peak rate
  float neededDps;       // the club's full-power rate
  float speed;           // impactDps / neededDps
  float yawDeg, twistDeg;// at impact (signs applied)
  float startDeg;        // launch direction offset (+ right)
  float curveDeg;        // curve (+ = to the right: slice for a right-hander)
  float quality;         // distance factor (over-swing, perfect)
  float meterPower;      // where the meter ended: peak / full (past 1 = past the line)
  SwingGrade grade;
  uint16_t downMs;       // how long the forward swing took
  bool perfect, overswing, tooSlow;   // overswing = grade Over; tooSlow is never set (too slow is no stroke)
  float followDeg, backspin;          // no backspin from a forward stroke (followDone at impact)
  bool followDone;
  float driftSide;       // an over-swing's drift: +1 right, -1 left (0: no over-swing)
};
constexpr uint32_t kFollowMs = 350;

class SwingAnalyzer {
 public:
  void begin(int club, const SwingTuning& tuning);
  SwingPhase feed(const SwingSample& s);   // Impact is returned once, then the result is final
  SwingPhase phase() const { return phase_; }
  // The meter, live: the fill is the forward swing's peak so far / full (0 at address), frozen at impact; red past
  // the over-swing line. There is no target any more (meterTarget() is 0).
  float meterTarget() const { return 0; }
  float meterFill() const;
  bool meterRed() const;
  // The shot's start direction and curve if it were hit now (from the latest yaw and twist), for the live line.
  void liveShape(float* startDeg, float* curveDeg) const;
  void liveFacePath(float* faceDeg, float* pathDeg) const;
  float liveTempo() const { return meterFill(); }
  float fullDps() const { return full_; }
  float countDps() const;
  float peakDps() const { return peak_; }
  const SwingResult& result() const { return r_; }

 private:
  void finish(const SwingSample& s);
  SwingTuning t_;
  bool putter_ = false;
  float full_ = 1;
  SwingPhase phase_ = SwingPhase::Address;
  float peak_ = 0;
  uint32_t startMs_ = 0, peakMs_ = 0;
  SwingSample prev_;
  bool havePrev_ = false;
  SwingResult r_;
};

// Reads a SwingTracker as signed swing / yaw / twist. Until the tracker has learned the swing plane (20 degrees),
// the rotation is projected on the axis of its first few degrees, so small putting strokes work too. The swing
// angle is unwrapped, so a backswing past 180 degrees keeps counting up (yaw and twist are only meaningful near
// address, where impact is judged).
class SwingReader {
 public:
  void begin();
  // allowFlip (pass true while the analyzer is still at address): a move of more than 4 degrees the other way
  // (e.g. a forward press) turns the direction round, so the backswing always reads positive.
  SwingSample read(const core::motion::SwingTracker& t, uint32_t ms, bool allowFlip = false);
 private:
  SwingSample unwrap(SwingSample s);
  bool haveAxis_ = false;
  core::motion::Vec3 axis_, fwd_;
  // The tracker reports the shortest rotation, so past 180 degrees the swing jumps from +180 to -180; the reader
  // keeps the angle continuous (a real backswing can go well past 180).
  bool haveLast_ = false;
  float lastRaw_ = 0, offset_ = 0;
  // Yaw (path, about the true vertical) and twist (tilt, about the screen axis) are added up sample by sample from
  // the kami's rate split three ways (swing / tilt / vertical), so they read the same at any point of the swing.
  float yaw_ = 0, twist_ = 0;
};

// A synthetic forward swing through the real analyzer (dev command, tests, sims): a bell-shaped rate peaking at
// `peakDps` over ~0.25 s about the swing axis, with `yawDeg` / `twistDeg` built up by the peak. 104 Hz samples.
SwingResult synthesizeSwing(float peakDps, float yawDeg, float twistDeg, int club, const SwingTuning& tuning);

// What the meter shows, live during the swing and frozen at impact. The app keeps it until the next address.
struct SwingFeedback {
  bool active;            // a swing is under way or done
  bool frozen;            // after impact
  uint8_t phase;          // 2 forward swing, 3 impact
  float fill, target;     // the meter (target is 0: no backswing)
  bool red;
  float tempo;            // = fill
  float faceDeg, pathDeg; // twist (+ open: slice) and yaw (+ right: push)
  float followDeg;
  bool followDone;
  float backspin;
  SwingGrade grade;
};
SwingFeedback swingFeedback(const SwingAnalyzer& a);

// ---------------- flight ----------------

struct Wind {
  float mph;
  float dirDeg;          // heading the wind blows toward (0 = toward the green = helping)
};
constexpr float kMphToYps = 0.48889f;

struct Point3 {
  float x, y, h;
};

enum class Outcome : uint8_t { Rest, Holed, Water, OutOfBounds, Unplayable };

struct ShotResult {
  Outcome outcome;
  float x, y;            // where the ball is now (rest, or the drop for water; the old spot for out of bounds)
  Surface lie;
  float landX, landY;    // first touchdown
  float carry, total;    // along the ground from the start
  float curveYd;         // sideways from the aim line at touchdown (+ right)
  float apex, flightS;
  bool hitTree;
  bool gimme;            // stopped within 2 ft and conceded
  int points;            // trajectory points written
};

struct ShotInput {
  float x, y;            // ball
  Surface lie;
  int club;
  float aimDeg;          // heading
  float putterRange;     // yards at 100 % for the putter
  SwingResult swing;
};

// Plays the shot on the hole. traj (optional) receives a point every `trajDt` seconds, flight and roll.
ShotResult playShot(const HoleMap& map, const Wind& wind, const ShotInput& in, Point3* traj, int trajMax,
                     float trajDt = 0.05f);

// Putter range for a putt of `dist` yards: 1.25 x the distance rounded up to 5 yd, 5..30.
float putterRangeFor(float dist);

// The game's cup (yards): a forgiving 0.4 yd, like Wii Sports (a real cup is 0.06). The ball drops when its centre
// rolls over it slowly enough, and it is drawn at exactly this size, so a putt that looks in is in.
constexpr float kCupRadius = 0.4f;
// The cup as drawn on a view of `scale` px per yard (radius, px; at least 1.5 px so it stays visible).
inline float cupRadiusPx(float scale) { return scale * kCupRadius > 1.5f ? scale * kCupRadius : 1.5f; }
constexpr float kGimme = 0.67f;

// The ground track the shot would fly (for the live curved aim line): kPreviewPoints points from the ball to the
// landing spot, from the same carry, start, curve and wind as playShot, without the per-shot solve (every frame).
constexpr int kPreviewPoints = 16;
int previewShot(const Wind& wind, int club, Surface lie, float x, float y, float aimDeg, const SwingResult& swing,
                float putterRange, Point3* out);

// The Shot Meter bends like Switch Sports': bend -1..1 (+ = right, the way the ball curves; 0 when square, more the
// further off); an over-swing overshoots above 100 % (overshoot, share of the bar) and whips to its drift side.
struct MeterBend {
  float bend;
  float overshoot;
  float overSide;
};
MeterBend meterBend(float startDeg, float curveDeg, float fill, bool over, float driftSide);

// Eases the live line toward the swing's reading: responsive (tau ~80 ms) but without frame-to-frame jitter.
float smoothToward(float current, float target, float dtMs, float tauMs);

// "Straight", "Draw", "Fade", "Hook", "Slice", "Push", "Pull" or "Over-swing".
const char* shotShape(const SwingResult& r);

// ---------------- caddie ----------------

struct Plan {
  int club;
  float aimDeg;
  float power;           // 0..1
  float targetX, targetY;
  float putterRange;
};

// A sensible shot: the pin if it is in reach and safe, otherwise the best lay-up along the route; compensates the
// wind. `readGreen` also reads the break on putts (the sims and `golf auto`; the player reads it themselves).
Plan caddie(const HoleMap& map, const Wind& wind, float x, float y, Surface lie, bool readGreen);

// ---------------- scoring ----------------

const char* scoreName(int strokes, int par);   // "Hole in one!", "Birdie", "Par", "Bogey", ...

// Heading from (x0, y0) to (x1, y1), degrees clockwise from +y.
float headingTo(float x0, float y0, float x1, float y1);

}  // namespace golf
}  // namespace core
