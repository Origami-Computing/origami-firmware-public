#pragma once
/**
 * Tennis spike (throwaway): pure game logic for a Wii-Sports-Resort-style tennis game on the kami.
 *
 * - SwingDetector: hold-agnostic swing detection. Calibration averages the gyro bias and the gravity direction
 *   while the device is held still; a swing is then rotation about the true vertical (gyro . up), so forehand and
 *   backhand are its sign, whatever the grip.
 * - Game: 2.5D court (x across -1..1, z depth 0 = player's baseline .. 1 = AI's, height h), serve with a toss whose
 *   apex rewards a power serve, rally with auto-positioning and auto-aim, a capped-speed AI, tennis scoring.
 *
 * No Arduino, no allocation: host-testable (test/test_tennis/test_host.cpp).
 */
#include <stdint.h>

namespace core {
namespace tennis {

enum class Side : int8_t { None = 0, Forehand = 1, Backhand = -1 };

struct SwingTuning {
  // 2026-09-30: all 30% lower than first tuned (the user: less movement for the same result)
  float startDps = 140.0f;     // |yaw rate| that starts a swing
  float endDps = 56.0f;        // falls back below this (or below endFraction of the peak): the swing is over
  float endFraction = 0.45f;   // report when the swing has slowed, not when the follow-through stops
  float minPeakDps = 210.0f;   // weaker bursts are ignored (wobble, backswing)
  uint16_t refractoryMs = 80;  // only stops one wobbly swing counting twice; the game ignores take-backs
  float forehandSign = 1.0f;   // +yaw (counter-clockwise from above) = forehand for a right-hander
  float serveDps = 175.0f;     // serve: total rotation rate, any direction
};

struct Swing {
  Side side;          // from the yaw sign; None if the yaw peak was under minPeakDps (e.g. an overhead serve)
  float peakDps;      // peak |yaw|: rally power
  float totalDps;     // peak |gyro|, any axis: serves
  uint32_t peakMs;    // when |gyro| peaked: the moment of contact
};

class SwingDetector {
 public:
  explicit SwingDetector(const SwingTuning& t = SwingTuning()) : t_(t) {}
  void calibrateBegin();
  void calibrateSample(const float accMg[3], const float gyroDps[3]);
  bool calibrateEnd();                         // false if too few samples or the device moved
  bool calibrated() const { return calibrated_; }
  // Feed one sample; true when a swing has finished, with its peak in *out.
  bool update(uint32_t nowMs, const float gyroDps[3], Swing* out);
  float lastYawDps() const { return yaw_; }
  float lastTotalDps() const { return total_; }
  const float* up() const { return up_; }

 private:
  SwingTuning t_;
  float bias_[3] = {0, 0, 0}, up_[3] = {0, 0, 1};
  float sumG_[3] = {0, 0, 0}, sumA_[3] = {0, 0, 0}, maxDev_ = 0;
  uint16_t n_ = 0;
  bool calibrated_ = false;
  bool inSwing_ = false;
  float yaw_ = 0, total_ = 0, peakYaw_ = 0, peakTotal_ = 0;
  uint32_t peakMs_ = 0, quietUntil_ = 0;
};

// ---- game ----

enum class Phase : uint8_t { ServeReady, Toss, Rally, PointOver, MatchOver };

struct Ball {
  float x, z, h;        // court position; h = height (court units, net = 0.12)
  float vx, vz, vh;     // per second
  uint8_t bounces;      // since the last hit
};

struct Player {
  float x;              // across
  float targetX;
};

struct Tuning {
  float gravity = 2.6f;          // court units / s^2
  float bounceKeep = 0.62f;      // vertical speed kept on a bounce
  float hitWindowMs = 230.0f;    // |swing peak - contact| that still hits
  float powerWindowMs = 70.0f;   // serve: |swing peak - toss apex| for a power serve
  float serveWindowMs = 230.0f;  // serve: beyond this it's a fault
  float playerSpeed = 1.1f;      // across units / s (auto-run)
  float playerReach = 0.12f;     // beyond the stance: further from the ball than this at contact = can't reach
  float aiSpeed = 1.3f;          // AI across speed
  float aiReactMs = 190.0f;      // before the AI starts moving
  float aiReach = 0.18f;         // how far from the ball it can still return it
  float aiError = 0.15f;         // aim noise (0 = perfect)
  float aiMissBase = 0.003f;     // chance the AI fluffs a ball it reached (+ up to aiMissPower for hard shots)
  float aiFlightS = 0.95f;       // AI shot flight time (+ up to 0.3 s); lower = faster, harder
  float aiMissPower = 0.015f;    // extra miss chance for a full-power shot
  // In position the AI aims 0.1 + up to aiWidth from the middle (landing x), but its ball crosses the player's
  // baseline (where the auto-run has to get to; the screen edge is ~0.98 there) at most aiWideX out, or up to
  // aiPunish wider when it gets a central ball (fading out by aiPassiveX from the middle): a passive shot gets
  // punished, a well-placed one doesn't give the AI the angle.
  float aiWidth = 0.9f;
  float aiWideX = 0.85f;
  float aiPunish = 0.12f;
  float aiPassiveX = 0.8f;
  // Aim from timing: t = (swing peak - contact - timingCenterMs) / aimSpanMs. Early (t < 0) pulls cross-court
  // (forehand left, backhand right), late pushes down the line; |t| = 1 reaches the sideline. The centre is the
  // user's median timing in the 2026-09-29 play log (hits were ~45 ms early on average).
  float timingCenterMs = -45.0f;
  float aimSpanMs = 110.0f;
  float aimSpread = 0.95f;       // |landing x| at |t| = 1
  float aimSafeT = 1.5f;         // slight auto-aim: up to this |t| the ball is kept inside the sidelines
  float aiRecover = 0.7f;        // while the ball is away, the AI drifts back to the middle at this x its speed
  float aiRecoverDelayMs = 250;  // after its shot, plus aiStretchDelayMs x stretch: a stretched AI recovers late
  float aiStretchDelayMs = 450;
  float tossApexS = 0.62f;       // seconds from toss to apex
  uint8_t gamesToWin = 3;
};

enum class Event : uint8_t {
  None, TossStarted, PowerServe, Serve, Fault, DoubleFault, Hit, Whiff, WrongSide, TooFar, AiHit, AiMiss, Out, Net,
  PlayerPoint, AiPoint, PlayerWinsMatch, AiWinsMatch
};

struct State {
  Phase phase;
  Ball ball;
  Player player, ai;
  uint8_t playerPoints, aiPoints;   // 0,1,2,3 = 0,15,30,40; >3 handled as deuce/advantage
  uint8_t playerGames, aiGames;
  uint8_t faults;                   // this serve
  bool playerServes;
  bool lastHitByPlayer;
  bool lastPointToPlayer;           // who won the last finished point
  uint32_t phaseMs;                 // when the phase began
  uint32_t aiMoveAtMs;              // AI reacts after this
  uint32_t contactMs;               // predicted time the ball reaches the player's hitting plane (0 = not coming)
  float lastPower;                  // 0..1, the last shot's power (harder shots are harder for the AI)
  uint32_t aiRecoverAtMs;           // the AI starts drifting back to the middle after this
  float aiStretch;                  // AI's last contact: |ai - ball| / reach (> 1 = couldn't reach it)
  int16_t lastTimingMs;             // the player's last swing vs contact / toss apex: < 0 early, > 0 late
  Event lastEvent;
  uint32_t rng;
};

class Game {
 public:
  explicit Game(const Tuning& t = Tuning()) : t_(t) {}
  void reset(uint32_t nowMs, uint32_t seed);
  // Advance to nowMs; returns the most important event this step (None if nothing happened).
  Event step(uint32_t nowMs);
  Event startToss(uint32_t nowMs);                 // player serving: C / button starts the toss
  Event swing(const Swing& sw, uint32_t nowMs);    // a detected swing
  // The game was paused (not stepped) for dtMs: move its clocks on so it carries on where it was.
  void shiftTime(uint32_t dtMs);
  const State& state() const { return s_; }
  State& mutableState() { return s_; }
  // Time (ms) the ball reaches the player's hitting plane, or 0 if it isn't coming to the player.
  uint32_t playerContactMs(uint32_t nowMs) const;
  Side sideNeeded() const;                         // forehand if the ball is (will be) right of the player
  // Where an incoming ball crosses the player's hitting plane (before or after it gets there); the ball's x if
  // it isn't coming.
  float contactX() const;
  const Tuning& tuning() const { return t_; }
  static const char* scoreText(uint8_t mine, uint8_t theirs, char* buf, int len);  // "15", "Deuce", "Ad"

 private:
  Event hitFrom(bool byPlayer, float targetX, float targetZ, float flightS, uint32_t nowMs);
  Event pointTo(bool player, uint32_t nowMs);
  Event endPoint(bool toPlayer, Event why, uint32_t nowMs);   // the point's reason, unless it ended the match
  Event fault(uint32_t nowMs);
  float rand01();
  Tuning t_;
  State s_{};
};

}  // namespace tennis
}  // namespace core
