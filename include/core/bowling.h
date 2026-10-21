#pragma once
/**
 * Bowling: pure game logic for a Wii-Sports-style motion bowling game on the kami.
 *
 * - SwingReader: B pressed = address; a SwingTracker learns the swing plane from the backswing; the forward swing is
 *   integrated in the world frame -> peak rate (speed), heading of the swing plane (line) and wrist twist about the
 *   true vertical (hook). Release on B up, or automatically when the forward swing has slowed.
 * - makeThrow: reading + dial aim + position -> ball speed, launch angle, spin.
 * - Lane: real-size lane (metres), oil pattern (hook grows late), gutters, 2D ball/pin physics with falling pins
 *   that knock their neighbours over. Deterministic, fixed 1 ms steps.
 * - Scorecard: ten-pin scoring with strikes, spares, the 10th frame, split flags. isSplit() for a leave.
 * - Match: a whole game: position/aim, throw, settle, score, next rack.
 *
 * Coordinates: x across the lane (0 = centre, + = right as the bowler sees it), y along it (0 = foul line, the head
 * pin at kLaneLen). Angles in degrees, + = to the right. No Arduino, no allocation: host-tested in
 * test/test_bowling/test_host.cpp.
 */
#include <stdint.h>

#include "core/swing_track.h"

namespace core {
namespace bowling {

// ---- geometry (metres) ----
constexpr float kLaneLen = 18.288f;      // foul line -> head pin (60 ft)
constexpr float kLaneHalfW = 0.527f;     // 41.5 in wide, 39 boards
constexpr float kBoard = 2 * kLaneHalfW / 39;
constexpr float kGutterW = 0.235f;
constexpr float kKickback = kLaneHalfW + kGutterW;   // the deck's side walls
constexpr float kPitY = kLaneLen + 0.868f;           // end of the pin deck
constexpr float kOilEnd = 12.19f;                    // 40 ft oil pattern
constexpr float kBallR = 0.108f;
constexpr float kPinR = 0.0605f;                     // at the belly
constexpr float kPinH = 0.381f;
constexpr float kPinSpacing = 0.3048f, kRowSpacing = 0.2639f;
constexpr int kPins = 10;
constexpr uint16_t kFullRack = 0x3FF;                // bit i = pin i+1

void pinSpot(int i, float* x, float* y);            // pin i = 0..9 (pin number - 1)
bool isSplit(uint16_t standing);                     // after a ball at a full rack

// ---- the throw ----
struct SwingReading {
  float peakDps;       // peak forward-swing rate
  float headingDeg;    // swing-plane heading relative to the backswing, + = right
  float twistDeg;      // wrist twist during the forward swing, + = clockwise from above (hook right)
  float backswingDeg;  // how far back the swing went
};

struct Throw {
  float speed;         // m/s
  float angleDeg;      // launch angle, + = right
  float spin;          // -1 (full hook left) .. 1 (full hook right)
  float startX;        // release position across the lane
};

float speedFor(float peakDps);
Throw makeThrow(const SwingReading& r, float aimDeg, float startX);

// ---- swing reading ----
enum class SwingStage : uint8_t { Idle, Address, Backswing, Forward };
enum class SwingResult : uint8_t { None, Throw, Tap, Cancel };

class SwingReader {
 public:
  SwingReader();
  void setBias(const float gyroDps[3]);
  void press(uint32_t ms, const float accMg[3]);          // B down: this pose is the address
  SwingResult sample(uint32_t ms, const float gyroDps[3]); // Throw when it releases by itself
  SwingResult release(uint32_t ms);                        // B up
  SwingStage stage() const { return stage_; }
  float backswingSoFar() const { return maxSwing_; }   // for logging a swing that didn't throw
  float peakSoFar() const { return peak_; }
  const SwingReading& reading() const { return reading_; }
  float armDeg() const;                                    // swing angle from address (back = +), for the animation
  // A+B while B is down: true = keep the game (a swing is under way: backswing started, or B held > kChordHoldMs);
  // false = let the system exit, after cancel() so B's release is not read as a tap.
  bool keepsChord(uint32_t ms) const;
  void cancel() { stage_ = SwingStage::Idle; }
  static constexpr uint32_t kChordHoldMs = 250;

 private:
  SwingResult finish();
  motion::SwingTracker tr_;
  SwingStage stage_;
  SwingReading reading_;
  uint32_t pressMs_, lastMs_;
  float maxAngle_, maxSwing_;
  float bp_, bu_, bf_;               // backswing: integrated plane / vertical / forward-axis rotation
  float fp_, fu_, ff_;               // forward swing: plane / vertical / forward-axis rotation
  float peak_;
};

// ---- lane physics ----
enum class PinState : uint8_t { Standing, Falling, Down, Gone };

struct Pin {
  float x, y, vx, vy;
  float tilt;          // 0 upright .. 90 lying
  float dir;           // fall direction, radians (atan2(dy, dx) in lane coordinates)
  float wobble;        // 1 after a gentle knock, decays
  PinState state;
  bool out;            // in the gutter: no more collisions
};

struct Ball {
  float x, y, vx, vy;
  float hook;          // lateral velocity still to gain (m/s, signed)
  float roll;          // distance rolled (m), for the finger holes
  bool gutter, gone, live;
};

class Lane {
 public:
  Lane();
  void rack(uint16_t standing);
  void launch(const Throw& t);
  void step(float dtS);               // fixed 1 ms substeps inside
  bool settled() const;
  uint16_t standing() const;          // mask of pins still standing
  const Ball& ball() const { return ball_; }
  const Pin& pin(int i) const { return pins_[i]; }
  bool touched() const { return firstHit_ >= 0; }
  float sinceHit() const { return firstHit_ >= 0 ? t_ - firstHit_ : -1; }   // s since the first pin was hit
  float time() const { return t_; }
  uint16_t racked() const { return racked_; }

 private:
  void substep();
  void knock(Pin& p, float vx, float vy);
  Ball ball_;
  Pin pins_[kPins];
  uint16_t racked_;
  float t_, firstHit_;
};

// ---- scoring ----
class Scorecard {
 public:
  Scorecard() { reset(); }
  void reset();
  bool roll(int pins, bool split);    // false if the game is already over
  bool over() const;
  int frame() const;                  // 0..9, the frame the next ball belongs to
  int ball() const;                   // 0..2, which ball of that frame
  int total(int f) const;             // cumulative score through frame f, -1 while bonuses are pending
  int score() const;                  // the latest known cumulative score
  void marks(int f, char out[4]) const;   // "X", "7/", "9-", "X81", ... (unrolled balls are ' ')
  bool splitAt(int f, int b) const;
  int rolls() const { return n_; }

 private:
  int start(int f) const;             // index of frame f's first roll, -1 if not reached
  uint8_t r_[21];
  bool split_[21];
  int n_;
};

// ---- a whole game ----
enum class Phase : uint8_t { Aiming, Rolling, Result, GameOver };
enum class Outcome : uint8_t { None, Strike, Spare, Split, Gutter, Count };

class Match {
 public:
  Match() { reset(); }
  void reset();
  void move(int boards);              // +- boards, clamped
  void aim(int clicks);               // 0.25 degree clicks, clamped
  void setPosition(int boards);
  void setAim(float deg);
  float startX() const { return pos_ * kBoard; }
  int position() const { return pos_; }
  float aimDeg() const { return aim_; }
  bool throwBall(const Throw& t);     // Aiming -> Rolling
  bool advance(float dtS);            // physics; true when the ball just settled (-> Result)
  void score(uint16_t standingAfter, bool gutter);   // Rolling -> Result (advance calls it; tests may too)
  void next();                        // Result -> Aiming (next rack) or GameOver
  Phase phase() const { return phase_; }
  Outcome outcome() const { return outcome_; }
  int lastCount() const { return count_; }
  uint16_t rack() const { return rack_; }
  const Lane& lane() const { return lane_; }
  const Scorecard& card() const { return card_; }

  static constexpr int kMaxBoards = 14;
  static constexpr float kAimStep = 0.25f, kMaxAim = 4.0f;

 private:
  Lane lane_;
  Scorecard card_;
  Phase phase_;
  Outcome outcome_;
  uint16_t rack_, after_;
  bool fresh_;                        // this ball is the first at a full rack
  int pos_, count_;
  float aim_;
};

}  // namespace bowling
}  // namespace core
