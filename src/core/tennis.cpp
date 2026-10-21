// Tennis spike (throwaway): see include/core/tennis.h.
#include "core/tennis.h"

#include <math.h>
#include <string.h>
#include <stdio.h>

namespace core {
namespace tennis {

namespace {
constexpr float kNetZ = 0.5f;
constexpr float kNetH = 0.12f;
constexpr float kHitH = 0.22f;           // contact height
constexpr float kAiZ = 1.0f;             // AI hitting plane
constexpr float kPlayerZ = 0.0f;         // player hitting plane
constexpr float kStance = 0.18f;         // stand this far beside the ball
constexpr float kAiMaxDepth = 0.33f;     // the AI's shortest ball lands at most this far in from the player's baseline
constexpr uint32_t kPointPauseMs = 1600;
constexpr uint32_t kAiServeDelayMs = 1400;
constexpr uint32_t kLateReportMs = 300;   // a swing is reported after its peak: wait before calling the ball past

float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
float moveToward(float v, float target, float maxStep) {
  const float d = target - v;
  return fabsf(d) <= maxStep ? target : v + (d > 0 ? maxStep : -maxStep);
}
}  // namespace

// ---------------- SwingDetector ----------------

void SwingDetector::calibrateBegin() {
  memset(sumG_, 0, sizeof(sumG_));
  memset(sumA_, 0, sizeof(sumA_));
  n_ = 0;
  maxDev_ = 0;
  calibrated_ = false;
  inSwing_ = false;
}

void SwingDetector::calibrateSample(const float accMg[3], const float gyroDps[3]) {
  for (int i = 0; i < 3; ++i) { sumG_[i] += gyroDps[i]; sumA_[i] += accMg[i]; }
  const float mag = sqrtf(gyroDps[0] * gyroDps[0] + gyroDps[1] * gyroDps[1] + gyroDps[2] * gyroDps[2]);
  if (mag > maxDev_) maxDev_ = mag;
  if (n_ < 60000) ++n_;
}

bool SwingDetector::calibrateEnd() {
  if (n_ < 8 || maxDev_ > 40.0f) return false;         // too few (the UI loop runs ~13 fps), or it moved
  float a[3], len = 0;
  for (int i = 0; i < 3; ++i) { bias_[i] = sumG_[i] / n_; a[i] = sumA_[i] / n_; len += a[i] * a[i]; }
  len = sqrtf(len);
  if (len < 500.0f) return false;                      // no clear gravity (free fall / bad data)
  for (int i = 0; i < 3; ++i) up_[i] = a[i] / len;      // a still accelerometer reads +1 g pointing up
  calibrated_ = true;
  quietUntil_ = 0;
  return true;
}

bool SwingDetector::update(uint32_t nowMs, const float gyroDps[3], Swing* out) {
  float g[3];
  for (int i = 0; i < 3; ++i) g[i] = gyroDps[i] - bias_[i];
  yaw_ = (g[0] * up_[0] + g[1] * up_[1] + g[2] * up_[2]) * t_.forehandSign;
  total_ = sqrtf(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
  if (!calibrated_) return false;
  if (!inSwing_) {
    if ((int32_t)(nowMs - quietUntil_) < 0 || total_ < t_.startDps) return false;
    inSwing_ = true;
    peakYaw_ = yaw_;
    peakTotal_ = total_;
    peakMs_ = nowMs;
    return false;
  }
  if (fabsf(yaw_) > fabsf(peakYaw_)) peakYaw_ = yaw_;
  if (total_ > peakTotal_) { peakTotal_ = total_; peakMs_ = nowMs; }
  if (total_ >= t_.endDps && total_ >= t_.endFraction * peakTotal_) return false;
  inSwing_ = false;
  quietUntil_ = nowMs + t_.refractoryMs;
  if (out) {
    out->side = fabsf(peakYaw_) >= t_.minPeakDps ? (peakYaw_ > 0 ? Side::Forehand : Side::Backhand) : Side::None;
    out->peakDps = fabsf(peakYaw_);
    out->totalDps = peakTotal_;
    out->peakMs = peakMs_;
  }
  return true;
}

// ---------------- Game ----------------

float Game::rand01() {
  s_.rng = s_.rng * 1664525u + 1013904223u;
  return (float)(s_.rng >> 8) / 16777216.0f;
}

void Game::reset(uint32_t nowMs, uint32_t seed) {
  memset(&s_, 0, sizeof(s_));
  s_.rng = seed ? seed : 1u;
  s_.playerServes = true;
  s_.phase = Phase::ServeReady;
  s_.phaseMs = nowMs;
  s_.ball = {-0.08f, kPlayerZ, kHitH, 0, 0, 0, 0};                 // in the server's free hand
}

uint32_t Game::playerContactMs(uint32_t) const {
  return (s_.phase == Phase::Rally && s_.ball.vz < 0) ? s_.contactMs : 0;
}

float Game::contactX() const {
  const Ball& b = s_.ball;
  return b.vz < 0 ? b.x + b.vx * ((b.z - kPlayerZ) / -b.vz) : b.x;
}

// Judged where the ball meets the racket: a swing is reported up to ~300 ms after its peak, by when an angled
// ball has flown on past the player.
Side Game::sideNeeded() const {
  return contactX() >= s_.player.x ? Side::Forehand : Side::Backhand;
}

void Game::shiftTime(uint32_t dtMs) {
  s_.phaseMs += dtMs;
  s_.aiMoveAtMs += dtMs;
  s_.aiRecoverAtMs += dtMs;
  if (s_.contactMs) s_.contactMs += dtMs;
}

Event Game::startToss(uint32_t nowMs) {
  if (s_.phase != Phase::ServeReady || !s_.playerServes) return Event::None;
  s_.phase = Phase::Toss;
  s_.phaseMs = nowMs;
  s_.ball = {s_.player.x - 0.08f, kPlayerZ, kHitH, 0, 0, t_.gravity * t_.tossApexS, 0};   // free hand
  return s_.lastEvent = Event::TossStarted;
}

Event Game::hitFrom(bool byPlayer, float tx, float tz, float flightS, uint32_t nowMs) {
  Ball& b = s_.ball;
  b.z = byPlayer ? kPlayerZ : kAiZ;
  b.vx = (tx - b.x) / flightS;
  b.vz = (tz - b.z) / flightS;
  b.vh = (0.0f - b.h + 0.5f * t_.gravity * flightS * flightS) / flightS;
  b.bounces = 0;
  s_.lastHitByPlayer = byPlayer;
  s_.phase = Phase::Rally;
  s_.phaseMs = nowMs;
  s_.contactMs = 0;
  if (byPlayer) s_.aiMoveAtMs = nowMs + (uint32_t)t_.aiReactMs;
  return Event::None;
}

Event Game::pointTo(bool player, uint32_t nowMs) {
  uint8_t& mine = player ? s_.playerPoints : s_.aiPoints;
  uint8_t& theirs = player ? s_.aiPoints : s_.playerPoints;
  ++mine;
  s_.lastPointToPlayer = player;
  s_.phase = Phase::PointOver;
  s_.phaseMs = nowMs;
  s_.faults = 0;
  s_.contactMs = 0;
  if (mine >= 4 && mine >= theirs + 2) {                 // game
    uint8_t& games = player ? s_.playerGames : s_.aiGames;
    ++games;
    s_.playerPoints = s_.aiPoints = 0;
    s_.playerServes = !s_.playerServes;
    if (games >= t_.gamesToWin) {
      s_.phase = Phase::MatchOver;
      return s_.lastEvent = player ? Event::PlayerWinsMatch : Event::AiWinsMatch;
    }
  } else if (mine >= 4 && theirs >= 4) {                 // keep deuce/advantage small: 3-3 is deuce
    --mine;
    --theirs;
  }
  return s_.lastEvent = player ? Event::PlayerPoint : Event::AiPoint;
}

Event Game::swing(const Swing& sw, uint32_t nowMs) {
  if (s_.phase == Phase::Toss) {
    const int32_t apexMs = (int32_t)(s_.phaseMs + (uint32_t)(t_.tossApexS * 1000.0f));
    const int32_t signedErr = (int32_t)(sw.peakMs - (uint32_t)apexMs);
    s_.lastTimingMs = (int16_t)(signedErr < -9999 ? -9999 : (signedErr > 9999 ? 9999 : signedErr));
    const float err = fabsf((float)signedErr);
    const float side = (s_.ai.x > 0 ? -1.0f : 1.0f);            // aim away from the AI
    if (err <= t_.powerWindowMs) {
      s_.lastPower = 1.0f;
      hitFrom(true, side * 0.55f, 0.68f, 0.72f, nowMs);
      return s_.lastEvent = Event::PowerServe;
    }
    if (err <= t_.serveWindowMs) {
      s_.lastPower = 0.4f;
      hitFrom(true, side * (0.2f + 0.3f * rand01()), 0.66f, 1.05f, nowMs);
      return s_.lastEvent = Event::Serve;
    }
    return fault(nowMs);
  }
  if (s_.phase != Phase::Rally || s_.ball.vz >= 0 || !s_.contactMs) return Event::None;
  const float err = (float)(int32_t)(sw.peakMs - s_.contactMs);   // < 0 early, > 0 late
  s_.lastTimingMs = (int16_t)(err < -9999 ? -9999 : (err > 9999 ? 9999 : err));
  if (sw.side == Side::None) return Event::None;                 // a wobble, not a stroke
  // Reported before the ball arrives: judge the player where the auto-run will have them at contact.
  const float atContact = contactX();
  const int32_t untilContact = (int32_t)(s_.contactMs - nowMs);
  const float playerX =
      untilContact > 0 ? moveToward(s_.player.x, s_.player.targetX, t_.playerSpeed * untilContact / 1000.0f) : s_.player.x;
  const Side needed = atContact >= playerX ? Side::Forehand : Side::Backhand;
  // An early swing the wrong way is the take-back before the real stroke: ignore it silently.
  if (sw.side != needed && err < 0) return Event::None;
  if (fabsf(err - t_.timingCenterMs) > t_.hitWindowMs) return s_.lastEvent = Event::Whiff;   // centred like the aim
  if (fabsf(atContact - playerX) > kStance + t_.playerReach) return s_.lastEvent = Event::TooFar;
  if (sw.side != needed) return s_.lastEvent = Event::WrongSide;
  const float power = clampf((sw.peakDps - 210.0f) / 420.0f, 0.0f, 1.0f);   // full at 630 dps (was 900)
  const float t = clampf((err - t_.timingCenterMs) / t_.aimSpanMs, -1.6f, 1.6f);
  const float pull = sw.side == Side::Forehand ? -1.0f : 1.0f;    // early forehand goes left, backhand right
  float tx = pull * -t * t_.aimSpread;
  if (fabsf(t) <= t_.aimSafeT) tx = clampf(tx, -t_.aimSpread, t_.aimSpread);   // only real mistiming goes wide
  const float angle = fminf(fabsf(t), 1.0f);
  s_.lastPower = power;
  s_.ball.x = atContact;                                          // the return starts at the racket
  s_.ball.h = kHitH;
  hitFrom(true, tx, 0.8f + 0.12f * power - 0.2f * angle, 1.2f - 0.45f * power, nowMs);   // angles land shorter
  return s_.lastEvent = Event::Hit;
}

Event Game::step(uint32_t nowMs) {
  Event ev = Event::None;
  switch (s_.phase) {
    case Phase::MatchOver:
      return Event::None;
    case Phase::PointOver:
      if ((int32_t)(nowMs - s_.phaseMs) >= (int32_t)kPointPauseMs) {
        s_.phase = Phase::ServeReady;
        s_.phaseMs = nowMs;
        s_.player.x = s_.playerServes ? 0.25f : 0.0f;          // the receiver waits in the middle
        s_.ai.x = s_.playerServes ? 0.0f : -0.25f;
        s_.ball = {s_.playerServes ? s_.player.x - 0.08f : s_.ai.x, s_.playerServes ? kPlayerZ : kAiZ, kHitH, 0, 0, 0, 0};
      }
      return Event::None;
    case Phase::ServeReady:
      if (!s_.playerServes && (int32_t)(nowMs - s_.phaseMs) >= (int32_t)kAiServeDelayMs) {
        s_.ball.x = s_.ai.x;
        s_.lastPower = 0.3f;
        hitFrom(false, (rand01() < 0.5f ? -0.45f : 0.45f), 0.3f, 1.1f, nowMs);
        return s_.lastEvent = Event::AiHit;
      }
      return Event::None;
    case Phase::Toss: {
      const float t = (float)(nowMs - s_.phaseMs) / 1000.0f;
      s_.ball.h = kHitH + s_.ball.vh * t - 0.5f * t_.gravity * t * t;
      if (s_.ball.h <= 0.0f) return fault(nowMs);             // let it drop: fault
      return Event::None;
    }
    case Phase::Rally:
      break;
  }

  // Rally: integrate in 10 ms steps since the last step (phaseMs doubles as "last integrated" in a rally).
  uint32_t elapsed = nowMs - s_.phaseMs;
  if (elapsed > 100) elapsed = 100;
  s_.phaseMs = nowMs;
  Ball& b = s_.ball;
  float left = elapsed / 1000.0f;
  while (left > 0 && s_.phase == Phase::Rally) {
    const float dt = left > 0.01f ? 0.01f : left;
    left -= dt;
    const float prevZ = b.z, prevH = b.h;
    b.x += b.vx * dt;
    b.z += b.vz * dt;
    b.vh -= t_.gravity * dt;
    b.h += b.vh * dt;
    // net
    if ((prevZ - kNetZ) * (b.z - kNetZ) <= 0 && prevZ != kNetZ) {
      const float f = (kNetZ - prevZ) / (b.z - prevZ);
      if (prevH + (b.h - prevH) * f < kNetH) return endPoint(!s_.lastHitByPlayer, Event::Net, nowMs);
    }
    // bounce
    if (b.h <= 0.0f && b.vh < 0) {
      b.h = 0.0f;
      b.vh = -b.vh * t_.bounceKeep;
      ++b.bounces;
      if (b.bounces == 1) {
        const bool inHalf = s_.lastHitByPlayer ? (b.z > kNetZ && b.z <= kAiZ) : (b.z < kNetZ && b.z >= kPlayerZ);
        if (!inHalf || fabsf(b.x) > 1.0f) return endPoint(!s_.lastHitByPlayer, Event::Out, nowMs);
      } else if (!(b.vz < 0 && b.z < kPlayerZ)) {             // second bounce before reaching the receiver
        return pointTo(s_.lastHitByPlayer, nowMs);
      }                                                       // behind the player: the late-swing timer decides
    }
  }
  if (s_.phase != Phase::Rally) return ev;

  if (b.vz < 0) {                                             // coming to the player
    if ((int32_t)(nowMs - s_.aiRecoverAtMs) >= 0)            // AI recovers (late if it was stretched)
      s_.ai.x = moveToward(s_.ai.x, 0.0f, t_.aiSpeed * t_.aiRecover * elapsed / 1000.0f);
    if (b.z > kPlayerZ) s_.contactMs = nowMs + (uint32_t)((b.z - kPlayerZ) / -b.vz * 1000.0f);
    const float atContact = contactX();
    s_.player.targetX = atContact >= s_.player.x ? atContact - kStance : atContact + kStance;
    s_.player.x = moveToward(s_.player.x, s_.player.targetX, t_.playerSpeed * elapsed / 1000.0f);
    if (s_.contactMs && (int32_t)(nowMs - s_.contactMs) > (int32_t)(t_.hitWindowMs + kLateReportMs) && b.bounces >= 1)
      return pointTo(false, nowMs);                           // it went past
  } else {                                                    // going to the AI
    if ((int32_t)(nowMs - s_.aiMoveAtMs) >= 0) {
      const float atContact = b.x + b.vx * ((kAiZ - b.z) / b.vz);
      s_.ai.targetX = atContact;
      s_.ai.x = moveToward(s_.ai.x, s_.ai.targetX, t_.aiSpeed * elapsed / 1000.0f);
    }
    if (b.z >= kAiZ && b.bounces == 1) {
      s_.aiStretch = fabsf(s_.ai.x - b.x) / t_.aiReach;
      const float missChance = t_.aiMissBase + t_.aiMissPower * s_.lastPower;
      if (s_.aiStretch > 1.0f || rand01() < missChance) return endPoint(true, Event::AiMiss, nowMs);
      // In position: attack away from the player. Stretched: a slower, shorter, more central defensive ball.
      const float q = 1.0f - s_.aiStretch * s_.aiStretch;
      const float away = s_.player.x > 0 ? -1.0f : 1.0f;
      float tx = away * (0.1f + t_.aiWidth * q * (0.5f + 0.5f * rand01())) + (rand01() - 0.5f) * 2.0f * t_.aiError;
      // Shorter when defending, but never so short that it bounces twice before the player's baseline (the
      // player can't come forward): after a bounce at tz the ball carries ~0.66 x (1 - tz) further.
      const float tz = fminf(0.12f + 0.15f * rand01() + 0.2f * (1.0f - q), kAiMaxDepth);
      const float flightS = t_.aiFlightS + 0.3f * rand01() + 0.35f * (1.0f - q);
      // The ball keeps going after the bounce: where it crosses the player's baseline is what the auto-run has to
      // reach, and it must stay on screen (see Tuning::aiWideX).
      const float passive = q * fmaxf(0.0f, 1.0f - fabsf(b.x) / t_.aiPassiveX);
      const float wide = t_.aiWideX + t_.aiPunish * passive;
      const float cx = b.x + (tx - b.x) / (1.0f - tz);
      if (fabsf(cx) > wide) tx = b.x + (clampf(cx, -wide, wide) - b.x) * (1.0f - tz);
      b.h = kHitH;
      s_.lastPower = 0.3f;
      hitFrom(false, clampf(tx, -1.15f, 1.15f), tz, flightS, nowMs);
      s_.aiRecoverAtMs = nowMs + (uint32_t)(t_.aiRecoverDelayMs + t_.aiStretchDelayMs * s_.aiStretch);
      return s_.lastEvent = Event::AiHit;
    }
  }
  return ev;
}

Event Game::endPoint(bool toPlayer, Event why, uint32_t nowMs) {
  const Event e = pointTo(toPlayer, nowMs);
  return s_.lastEvent = (e == Event::PlayerWinsMatch || e == Event::AiWinsMatch) ? e : why;
}

Event Game::fault(uint32_t nowMs) {
  if (++s_.faults >= 2) return endPoint(false, Event::DoubleFault, nowMs);
  s_.phase = Phase::ServeReady;
  s_.phaseMs = nowMs;
  s_.ball.h = kHitH;
  return s_.lastEvent = Event::Fault;
}

const char* Game::scoreText(uint8_t mine, uint8_t theirs, char* buf, int len) {
  static const char* kNames[] = {"0", "15", "30", "40"};
  if (mine >= 3 && theirs >= 3) snprintf(buf, len, "%s", mine == theirs ? "Deuce" : (mine > theirs ? "Ad" : "40"));
  else snprintf(buf, len, "%s", kNames[mine > 3 ? 3 : mine]);
  return buf;
}

}  // namespace tennis
}  // namespace core
