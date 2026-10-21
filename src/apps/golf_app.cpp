/**
 * @file golf_app.cpp
 * @brief Golf: three nine-hole courses and a driving range, played by swinging the kami (Wii Sports Resort style).
 *
 * Hold B to address (that pose is neutral), tilt the top toward you for the backswing (the top sets the power),
 * swing back through the address pose to hit. The dial aims, A / C change club. See the spec:
 * docs/superpowers/specs/2026-09-30-golf-design.md.
 *
 * Game logic, courses and flight: core/golf, core/golf_course (host-tested). Pictures: ui/golf_render (the hole,
 * rendered a band of rows per tick into PSRAM) and ui/golf_draw (each frame, straight into the canvas buffer).
 * This file reads the IMU and the buttons, runs the flow and fills a Scene. Everything big lives in PSRAM while
 * the app is open; static RAM is one pointer.
 */

#if defined(MENU_APP) && defined(KAMI_GAMES) && KAMI_GAMES  // N16R8 only (platformio.ini)

#include "apps/golf_app.h"
#include "core/golf.h"
#include "core/golf_course.h"
#include "ui/golf_draw.h"
#include "ui/golf_help.h"
#include "ui/golf_render.h"
#include "core/health_policy.h"
#include "core/swing_track.h"
#include "services/sports_motion.h"
#include "ui/screen_chrome.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <string.h>

#include "hal/hal_display.h"
#include "services/vibration.h"
#include "scene/screen.h"
#include "ui/scene.h"

namespace apps {
namespace golf {

namespace G = core::golf;
namespace GV = ui::golf;
namespace MI = services::motion_input;

namespace {

constexpr int kMapW = 280, kMapMaxH = 470, kAuxW = 280, kAuxH = 202, kTrajMax = 640;
constexpr uint32_t kRenderBudgetUs = 45000;     // per tick while you can play (the UI loop runs ~12 fps)
constexpr uint32_t kLoadBudgetUs = 75000;       // per tick behind the intro / title, where the screen is still

enum class Mode : uint8_t { Title, CourseMenu, Intro, Aim, Swing, Flight, Banner, Card, Overview };
enum class Aux : uint8_t { None, Title, Overview, Green };
enum class Job : uint8_t { None, Title, Map, Green };

struct Game {
  Mode mode;
  uint32_t activeAt, frame, modeAt;
  // menus
  int8_t menuSel, courseSel;
  // calibration
  bool imu, calibrated, calibrating;
  // round
  int8_t course, hole;
  bool practice;                 // a single hole (or the range)
  uint8_t strokes[9];
  int stroke;                    // strokes on this hole so far
  uint32_t roundSeed;
  G::HoleMap map;
  G::Wind wind;
  float ballX, ballY;
  G::Surface lie;
  int club;
  float aimDeg;
  float putterRange;
  // pictures
  uint16_t* mapBuf;
  GV::View mapView;
  uint16_t* aux;
  GV::View auxView;
  int auxW, auxH;
  Aux auxKind;
  Job job;
  GV::HoleRenderer renderer;
  float camY;
  // swing
  G::SwingTuning tuning;
  core::motion::SwingTracker tracker;
  G::SwingReader reader;
  G::SwingAnalyzer analyzer;
  bool bDown;
  bool tapOk;                    // the last B press was a quick tap at address: its B_Tap opens the overview
  uint32_t bDownAt, stillSince;
  float lastSwing;
  G::SwingResult swing;
  uint32_t impactAt;
  // flight
  G::Point3* traj;
  G::ShotResult shot;
  uint32_t flightAt;
  float flightSpeed;
  bool afterShotHandled;
  // messages
  char banner[28], sub[44], lines[4][40];
  uint16_t bannerColor;
  uint32_t bannerUntil;
  Mode afterBanner;
  // Wii-style swing: live line, practice swings
  bool swingOnly;                // Swing Practice: swings are measured and shown, nothing is hit
  bool practiceMode;             // A while aiming: swings (hold B as usual) show the meter and line, nothing is hit
  uint32_t pStillSince, pSwingAt;
  G::Point3 livePts[G::kPreviewPoints];
  G::Point3 frozen[G::kPreviewPoints];
  int frozenN;
  uint32_t frozenUntil;          // 0 with frozenN: until the next swing
  char practiceMsg[48];
  uint32_t practiceMsgUntil;
  bool pendingPractice;
  G::SwingResult devPractice;
  GV::HelpController help;        // hold A: how to play
  // the Shot Meter and feedback bars: live from the backswing, frozen at impact, up until the next address
  G::SwingFeedback fb;
  G::SwingFeedback fbShown;      // the empty meter shown at address
  bool fbOn;
  float fbCarry;                 // the swung club's full carry (the meter's distance labels)
  uint16_t* fbCache;             // PSRAM: the drawn column, redrawn only when a value changes
  uint32_t fbKey;
  bool following;                // still reading the follow-through after impact
  float liveStart, liveCurve;    // the live line (smoothed toward the swing's reading)
  uint32_t liveAt, previewUs;
  float shotX, shotY;            // where the last shot was hit from (the backspin re-run)
  G::Surface shotLie;
  uint8_t followKind;            // 1 a real shot, 2 practice mode, 3 Swing Practice
  // drawing
  GV::Scene scene;
  uint8_t cardPars[9], cardOne[9];
  // dev
  bool pendingSwing;
  bool trace;                    // dev: print the swing reading every ~250 ms while B is held
  uint32_t traceAt;
  G::SwingResult devSwing;
  uint32_t drawUs, tickUs, renderMsTotal, jobStartMs;
  GV::DrawTiming timing;
  uint32_t maxDrawUs;
};

Game* g = nullptr;

const char* const kCourseItems[] = {"Willow Park", "Gull Point Links", "Red Mesa Canyon", "Driving Range",
                                    "Swing Practice"};
constexpr int kTitleItems = 5;
const char* const kHoleItems[] = {"Play 9 holes", "Practice hole 1", "Practice hole 2", "Practice hole 3",
                                  "Practice hole 4", "Practice hole 5", "Practice hole 6", "Practice hole 7",
                                  "Practice hole 8", "Practice hole 9"};

uint32_t mix32(uint32_t h) {
  h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
  return h;
}

void buzz(uint32_t ms) {
  services::vibration::pulse(services::vibration::Owner::Golf, ms);
}

// Golf is waiting for the player (the "how to play" panel may open): not mid-swing, mid-flight or mid-message.
bool waiting() {
  switch (g->mode) {
    case Mode::Title: case Mode::CourseMenu: case Mode::Intro: case Mode::Card: case Mode::Overview: return true;
    case Mode::Aim: return !g->bDown;
    case Mode::Banner: return g->bannerUntil == 0;
    default: return false;
  }
}

void setMode(Mode m) {
  g->mode = m;
  g->modeAt = millis();
}

void say(const char* banner, const char* sub, uint16_t color, uint32_t ms, Mode after) {
  snprintf(g->banner, sizeof(g->banner), "%s", banner);
  snprintf(g->sub, sizeof(g->sub), "%s", sub ? sub : "");
  g->bannerColor = color;
  g->bannerUntil = ms ? millis() + ms : 0;
  g->afterBanner = after;
  setMode(Mode::Banner);
}

// The feedback bars go away (a new hole, the title, practice mode on/off, a cancelled swing).
void clearFeedback() {
  g->fbOn = false;
  g->following = false;
}

bool isRange() { return g->course == 3; }
const G::HoleDef& holeDef() { return G::course(g->course).holes[g->hole]; }
float toPin() { return sqrtf((g->map.pinX() - g->ballX) * (g->map.pinX() - g->ballX) + (g->map.pinY() - g->ballY) * (g->map.pinY() - g->ballY)); }
bool putter() { return g->club == G::kPutter; }

// ---------------- pictures ----------------

void startJob(Job job) {
  g->job = job;
  g->jobStartMs = millis();
  g->renderMsTotal = 0;
  if (job == Job::Map) {
    g->mapView = GV::holeView(g->map, kMapW, kMapMaxH, 1.2f);
    g->renderer.begin(&g->map, g->mapView, g->mapBuf, kMapW, false);
  } else if (job == Job::Green) {
    g->auxView = GV::greenView(g->map, kAuxW, kAuxH);
    g->auxW = kAuxW;
    g->auxH = kAuxH;
    g->auxKind = Aux::None;
    g->renderer.begin(&g->map, g->auxView, g->aux, kAuxW, true);
  } else if (job == Job::Title) {
    g->auxView = GV::greenView(g->map, kAuxW, kAuxH);
    g->auxW = kAuxW;
    g->auxH = kAuxH;
    g->auxKind = Aux::None;
    g->renderer.begin(&g->map, g->auxView, g->aux, kAuxW, false);
  }
}

void makeOverview() {
  int ow = 0;
  g->auxView = GV::makeOverview(g->mapBuf, g->mapView, g->aux, 130, kAuxH - 8, &ow);
  g->auxW = ow;
  g->auxH = kAuxH - 8;
  g->auxKind = Aux::Overview;
}

void runJobs() {
  if (g->job == Job::None) return;
  const uint32_t t0 = micros();
  const bool still = g->mode == Mode::Intro || g->mode == Mode::Title || g->mode == Mode::CourseMenu;
  const uint32_t budget = still ? kLoadBudgetUs : kRenderBudgetUs;
  while (!g->renderer.done() && (uint32_t)(micros() - t0) < budget) g->renderer.step(2);
  g->renderMsTotal += (micros() - t0) / 1000;
  if (!g->renderer.done()) return;
  const Job finished = g->job;
  g->job = Job::None;
  Serial.printf("[golf] render %u done: %u ms of ticks, %u ms wall\n", (unsigned)finished, (unsigned)g->renderMsTotal,
                (unsigned)(millis() - g->jobStartMs));
  g->renderMsTotal = 0;
  if (finished == Job::Map) makeOverview();
  else if (finished == Job::Green) g->auxKind = Aux::Green;
  else if (finished == Job::Title) g->auxKind = Aux::Title;
}

bool wantGreenView() {
  if (g->lie != G::Surface::Green && g->lie != G::Surface::Fringe) return false;
  const GV::View v = GV::greenView(g->map, kAuxW, kAuxH);
  const float x = GV::viewX(v, g->ballX), y = GV::viewY(v, g->ballY);
  return x > 10 && x < kAuxW - 10 && y > 10 && y < kAuxH - 30;
}

void titleBackdrop() {
  const int c = g->courseSel > 3 ? 3 : g->courseSel;         // Swing Practice shows the range
  g->map.load(G::course(c).holes[0], G::course(c).theme, (uint32_t)c * 100u);
  startJob(Job::Title);
}

// ---------------- the round ----------------

void planShot() {
  if (isRange()) {
    g->aimDeg = 0;
    g->putterRange = 20;
    if (g->club == G::kPutter) g->club = G::kDriver;
    return;
  }
  const uint32_t t0 = micros();
  const G::Plan p = G::caddie(g->map, g->wind, g->ballX, g->ballY, g->lie, false);
  g->club = p.club;
  g->aimDeg = p.club == G::kPutter ? G::headingTo(g->ballX, g->ballY, g->map.pinX(), g->map.pinY()) : p.aimDeg;
  g->putterRange = G::putterRangeFor(toPin());
  Serial.printf("[golf] caddie %s aim %.1f power %.2f (%u us)\n", G::club(p.club).shortName, p.aimDeg, p.power,
                (unsigned)(micros() - t0));
}

void startHole(int course, int hole, bool practice) {
  clearFeedback();
  g->course = (int8_t)course;
  g->hole = (int8_t)hole;
  g->practice = practice;
  const G::CourseDef& c = G::course(course);
  g->map.load(c.holes[hole], c.theme, (uint32_t)course * 100u + (uint32_t)hole);
  const uint32_t h = mix32(g->roundSeed + (uint32_t)hole * 7919u + (uint32_t)course * 104729u);
  g->wind.mph = c.windMinMph + (float)((h & 0xFF) / 255.0) * (c.windMaxMph - c.windMinMph);
  g->wind.dirDeg = (float)((h >> 8) % 360);
  g->stroke = 0;
  g->ballX = holeDef().teeX;
  g->ballY = holeDef().teeY;
  g->lie = G::Surface::Tee;
  planShot();
  startJob(Job::Map);
  g->camY = 1e9f;
  setMode(Mode::Intro);
  Serial.printf("[golf] %s hole %d par %d %.0f yd wind %.0f mph @%.0f trees %d\n", c.name, hole + 1, holeDef().par,
                g->map.yards(), g->wind.mph, g->wind.dirDeg, g->map.treeCount());
}

void startRound(int course, int firstHole, bool practice) {
  memset(g->strokes, 0, sizeof(g->strokes));
  g->swingOnly = false;
  g->frozenN = 0;
  g->roundSeed = esp_random();
  startHole(course, firstHole, practice);
}

// Swing Practice: the range, where each swing shows the meter, the line and a card, and nothing is hit.
void startPractice() {
  startRound(3, 0, true);
  g->swingOnly = true;
}

void backToTitle() {
  clearFeedback();
  setMode(Mode::Title);
  titleBackdrop();
}

void holeOver() {
  const int par = holeDef().par;
  if (!g->practice) g->strokes[g->hole] = (uint8_t)(g->stroke > 255 ? 255 : g->stroke);
  char sub[44];
  snprintf(sub, sizeof(sub), "%d stroke%s, par %d", g->stroke, g->stroke == 1 ? "" : "s", par);
  const int d = g->stroke - par;
  const uint16_t col = d < 0 ? GV::gfx::rgb(255, 150, 140) : (d == 0 ? GV::gfx::rgb(255, 255, 255) : GV::gfx::rgb(255, 214, 90));
  say(G::scoreName(g->stroke, par), sub, col, 0, Mode::Card);
  buzz(d < 0 ? 220 : 60);
}

void describeSwing(const G::SwingResult& r, char* grade, size_t n) {
  if (r.overswing) snprintf(grade, n, "Overswing!");
  else if (r.perfect) snprintf(grade, n, "Perfect!");
  else if (r.tooSlow) snprintf(grade, n, "Too slow");
  else if (fabsf(r.curveDeg) >= fabsf(r.startDeg) && fabsf(r.curveDeg) > 1.5f) snprintf(grade, n, r.curveDeg > 0 ? "Slice" : "Hook");
  else if (fabsf(r.startDeg) > 1.5f) snprintf(grade, n, r.startDeg > 0 ? "Push" : "Pull");
  else snprintf(grade, n, "Nice shot");
}

uint16_t shapeColor(float start, float curve, bool over) {
  if (over) return GV::gfx::rgb(255, 100, 90);                              // over-swing: red
  if (fabsf(start) < 1.5f && fabsf(curve) < 1.5f) return 0;                // straight: white
  if (fabsf(curve) >= fabsf(start)) return curve > 0 ? GV::gfx::rgb(255, 180, 90) : GV::gfx::rgb(120, 190, 255);
  return GV::gfx::rgb(255, 230, 120);                                       // push / pull
}

// Keep the line the swing would have flown on screen (frozen at impact).
void freezeLine(const G::SwingResult& r, uint32_t forMs) {
  g->frozenN = G::previewShot(g->swingOnly ? G::Wind{0, 0} : g->wind, g->club, g->lie, g->ballX, g->ballY, g->aimDeg, r,
                              g->putterRange, g->frozen);
  g->frozenUntil = forMs ? millis() + forMs : 0;
}

// A swing that hits nothing: a practice swing in play (no B), or any swing in Swing Practice.
void practiceResult(const G::SwingResult& r) {
  g->swing = r;
  G::ShotInput in;
  in.x = g->ballX; in.y = g->ballY; in.lie = g->lie; in.club = g->club; in.aimDeg = g->aimDeg;
  in.putterRange = g->putterRange; in.swing = r;
  const G::ShotResult pr = G::playShot(g->map, g->swingOnly ? G::Wind{0, 0} : g->wind, in, nullptr, 0);
  freezeLine(r, g->swingOnly ? 0 : 4000);
  g->activeAt = millis();
  Serial.printf("[golf] practice %s: meter %.2f (target %.2f) grade %u speed %.2f face %.1f path %.1f -> carry %.0f total %.0f\n",
                G::shotShape(r), r.meterPower, r.power, (unsigned)r.grade, r.speed, r.twistDeg, r.yawDeg, pr.carry, pr.total);
  // one short line in the bottom strip: the shape and how far (the line on the course shows the rest)
  char dist[16];
  if (g->club == G::kPutter) snprintf(dist, sizeof(dist), "%.1f yd", pr.total);
  else snprintf(dist, sizeof(dist), "%d yd", (int)lroundf(pr.total));
  snprintf(g->practiceMsg, sizeof(g->practiceMsg), "%s%s  %s%s", g->swingOnly ? "" : "practice: ",
           r.perfect ? "Perfect!" : G::shotShape(r), dist, r.backspin > 0.5f ? "  Backspin!" : "");
  g->practiceMsgUntil = millis() + (g->swingOnly ? 600000u : 4000u);   // Swing Practice: until the next swing
}

void hit(const G::SwingResult& r) {
  g->swing = r;
  g->shotX = g->ballX;
  g->shotY = g->ballY;
  g->shotLie = g->lie;
  G::ShotInput in;
  in.x = g->ballX;
  in.y = g->ballY;
  in.lie = g->lie;
  in.club = g->club;
  in.aimDeg = g->aimDeg;
  in.putterRange = g->putterRange;
  in.swing = r;
  const uint32_t t0 = micros();
  freezeLine(r, 1800);                                    // the line it was meant to fly, while it flies
  g->shot = G::playShot(g->map, g->wind, in, g->traj, kTrajMax, 0.05f);
  ++g->stroke;
  g->impactAt = millis();
  g->flightAt = millis();
  g->flightSpeed = g->club == G::kPutter ? 1.0f : 1.6f;
  g->afterShotHandled = false;
  g->activeAt = millis();
  buzz(r.perfect ? 90 : 40);
  setMode(Mode::Flight);
  Serial.printf("[golf] shot %s power %.2f q %.2f top %.0f impact %.0f/%.0f dps yaw %.1f twist %.1f -> out %d carry %.1f "
                "total %.1f curve %.1f lie %s pts %d (%u us)\n",
                G::club(g->club).shortName, r.power, r.quality, r.topDeg, r.impactDps, r.neededDps, r.yawDeg, r.twistDeg,
                (int)g->shot.outcome, g->shot.carry, g->shot.total, g->shot.curveYd, G::surfaceName(g->shot.lie),
                g->shot.points, (unsigned)(micros() - t0));
}

// The follow-through has been read (kFollowMs after impact): a real shot gets its backspin (only the bounces change,
// so the flight on screen carries on seamlessly); a practice swing gets its readout / card now.
void followDone() {
  const G::SwingResult& r = g->analyzer.result();
  g->swing.followDeg = r.followDeg;
  g->swing.backspin = r.backspin;
  g->swing.followDone = true;
  Serial.printf("[golf] follow-through %.0f deg, backspin %.2f\n", r.followDeg, r.backspin);
  if (g->followKind == 1) {
    if (g->club != G::kPutter && r.backspin > 0.02f && g->mode == Mode::Flight && !g->afterShotHandled) {
      G::ShotInput in;
      in.x = g->shotX; in.y = g->shotY; in.lie = g->shotLie; in.club = g->club; in.aimDeg = g->aimDeg;
      in.putterRange = g->putterRange; in.swing = g->swing;
      g->shot = G::playShot(g->map, g->wind, in, g->traj, kTrajMax, 0.05f);
      Serial.printf("[golf]   with backspin: total %.1f lie %s\n", g->shot.total, G::surfaceName(g->shot.lie));
    }
  } else {
    practiceResult(g->swing);
  }
}

void nextShot() {
  if (g->stroke >= 10 && !isRange()) {                    // pick up
    g->stroke = 10;
    holeOver();
    return;
  }
  planShot();
  if (wantGreenView()) {
    if (g->auxKind != Aux::Green) startJob(Job::Green);
  }
  g->camY = 1e9f;
  setMode(Mode::Aim);
}

void afterFlight() {
  g->afterShotHandled = true;
  const G::ShotResult& s = g->shot;
  char grade[20], sub[44];
  describeSwing(g->swing, grade, sizeof(grade));
  if (isRange()) {                                        // one short line, a moment to see it, then the next ball
    g->ballX = s.outcome == G::Outcome::OutOfBounds ? g->ballX : s.x;
    g->ballY = s.outcome == G::Outcome::OutOfBounds ? g->ballY : s.y;
    snprintf(g->practiceMsg, sizeof(g->practiceMsg), "%s  %d yd%s", g->swing.perfect ? "Perfect!" : G::shotShape(g->swing),
             (int)lroundf(s.total), g->swing.backspin > 0.5f ? "  Backspin!" : "");
    g->practiceMsgUntil = millis() + 4000;
    say("", nullptr, 0, 1800, Mode::Aim);
    return;
  }
  switch (s.outcome) {
    case G::Outcome::Holed:
      g->ballX = g->map.pinX();
      g->ballY = g->map.pinY();
      holeOver();
      return;
    case G::Outcome::Water:
      ++g->stroke;
      g->ballX = s.x; g->ballY = s.y; g->lie = s.lie;
      snprintf(sub, sizeof(sub), "%s  +1 stroke, drop", grade);
      {
        const G::Point3& end = g->traj[s.points > 0 ? s.points - 1 : 0];
        const bool canyon = s.points > 0 && g->map.surfaceAt(end.x, end.y) == G::Surface::Canyon;
        say(canyon ? "Into the canyon!" : "Water!", sub, GV::gfx::rgb(140, 200, 255), 2200, Mode::Aim);
      }
      buzz(150);
      return;
    case G::Outcome::OutOfBounds:
      ++g->stroke;
      snprintf(sub, sizeof(sub), "%s  +1 stroke, replay", grade);
      say("Out of bounds", sub, GV::gfx::rgb(255, 170, 120), 2200, Mode::Aim);
      buzz(150);
      return;
    default:
      break;
  }
  g->ballX = s.x;
  g->ballY = s.y;
  g->lie = s.lie;
  if (s.gimme) {
    ++g->stroke;
    g->ballX = g->map.pinX();
    g->ballY = g->map.pinY();
    say("Gimme", "tapped in", GV::gfx::rgb(255, 255, 255), 1300, Mode::Banner);   // Banner: the hole result next
    return;
  }
  // one short line in the bottom strip, then straight on to the next shot
  if (g->club == G::kPutter)
    snprintf(g->practiceMsg, sizeof(g->practiceMsg), "%.1f yd left", toPin());
  else
    snprintf(g->practiceMsg, sizeof(g->practiceMsg), "%s  %s  %d yd%s%s", G::surfaceName(s.lie),
             g->swing.perfect ? "Perfect!" : G::shotShape(g->swing), (int)lroundf(s.total),
             g->swing.backspin > 0.5f ? "  Backspin!" : "", s.hitTree ? "  (tree)" : "");
  g->practiceMsgUntil = millis() + 4000;
  (void)sub;
  nextShot();
}

void bannerDone() {
  const Mode next = g->afterBanner;
  g->lines[0][0] = g->lines[1][0] = g->lines[2][0] = g->lines[3][0] = 0;
  if (next == Mode::Banner) {                               // after a gimme
    holeOver();
    return;
  }
  if (next == Mode::Card) {
    setMode(Mode::Card);
    return;
  }
  if (isRange()) {                                          // next ball from the tee
    g->ballX = holeDef().teeX;
    g->ballY = holeDef().teeY;
    g->lie = G::Surface::Tee;
    g->camY = 1e9f;
    setMode(Mode::Aim);
    return;
  }
  nextShot();
}

void cardDone() {
  if (g->practice) {                                        // replay the same hole
    startHole(g->course, g->hole, true);
    return;
  }
  if (g->hole + 1 < G::course(g->course).holeCount) {
    startHole(g->course, g->hole + 1, false);
    return;
  }
  backToTitle();
}

// ---------------- swing ----------------

// drainFifo: drop motion from before the B press. Not from inside the sample loop (re-taking the neutral while
// held still), where polling would overwrite the samples being read.
void address(bool drainFifo) {
  if (drainFifo) MI::poll(millis());
  g->tracker.begin();
  g->tracker.setBias(MI::bias());
  float acc[3];
  if (!(MI::readAccel(acc) && g->tracker.setUp(acc))) g->tracker.setUp(MI::upMg());
  g->reader.begin();
  g->analyzer.begin(g->club, g->tuning);
  g->stillSince = millis();
  g->lastSwing = 0;
}

// Swings are taken with B held: that pose is neutral, then one fast forward swing is the stroke (power = how hard;
// path and tilt steer). In practice mode (A while aiming) and in Swing Practice it shows the meter, the curved line and
// a readout, and nothing is hit.
void swingTick(uint32_t now) {
  const int n = MI::poll(now);
  const MI::Sample* s = MI::samples();
  if (g->calibrating) {                                   // a quiet background calibration: it never blocks play
    const MI::Calibration c = MI::calibrateStep(now);
    if (c == MI::Calibration::Done) {
      g->calibrating = false;
      g->calibrated = true;
      const float* b = MI::bias();
      const float* u = MI::upMg();
      Serial.printf("[golf] calibrated bias %.2f %.2f %.2f up %.0f %.0f %.0f\n", b[0], b[1], b[2], u[0], u[1], u[2]);
    }
  }
  if (g->help.isOpen()) return;                           // the panel is up: no swings
  if (g->following) {                                     // after impact: how far does the swing carry on?
    for (int i = 0; i < n && g->following; ++i) {
      g->tracker.update(s[i].dps, 1.0f / 104.0f);
      g->analyzer.feed(g->reader.read(g->tracker, s[i].ms, false));
      if (g->analyzer.result().followDone) {
        g->following = false;
        followDone();
      }
    }
    return;
  }
  if (g->mode != Mode::Swing) return;
  for (int i = 0; i < n; ++i) {
    g->tracker.update(s[i].dps, 1.0f / 104.0f);
    const G::SwingSample ss = g->reader.read(g->tracker, s[i].ms, false);   // forward stroke: either way
    const G::SwingPhase p = g->analyzer.feed(ss);
    g->lastSwing = ss.swing;
    // No re-zeroing at address any more: a tilt held before the swing aims the shot (Switch Sports), and a stroke is
    // a fast forward swing, so slow drift can't become one.
    if (p != G::SwingPhase::Address) g->activeAt = now;
    if (g->trace && (int32_t)(s[i].ms - g->traceAt) >= 250) {
      g->traceAt = s[i].ms;
      const core::motion::Vec3 r = g->tracker.rateDps();
      Serial.printf("[golf] trace phase %u swing %.1f yaw %.1f twist %.1f angle %.1f rate %.1f raw %.2f %.2f %.2f\n",
                    (unsigned)p, ss.swing, ss.yaw, ss.twist, g->tracker.angleDeg(),
                    sqrtf(r.x * r.x + r.y * r.y + r.z * r.z), s[i].dps[0], s[i].dps[1], s[i].dps[2]);
    }
    if (p == G::SwingPhase::Impact) {
      g->following = true;                                 // keep reading: the follow-through (backspin)
      if (g->swingOnly || g->practiceMode) {               // practice: show it, hit nothing (readout after the follow)
        g->swing = g->analyzer.result();
        g->followKind = g->swingOnly ? 3 : 2;
        freezeLine(g->swing, g->swingOnly ? 0 : 6000);
        setMode(Mode::Aim);
      } else {
        g->followKind = 1;
        hit(g->analyzer.result());
      }
      for (int j = i + 1; j < n && g->following; ++j) {  // the rest of this batch is follow-through
        g->tracker.update(s[j].dps, 1.0f / 104.0f);
        g->analyzer.feed(g->reader.read(g->tracker, s[j].ms, false));
        if (g->analyzer.result().followDone) {
          g->following = false;
          followDone();
        }
      }
      return;
    }
  }
}

// ---------------- camera ----------------

void updateCamera(float targetY, bool snap) {
  const float want = (float)GV::followCam(g->mapView, g->mapView.h, g->ballY, targetY);
  if (snap || g->camY > 1e8f) g->camY = want;
  else g->camY += (want - g->camY) * 0.35f;
}

void flightBall(float* x, float* y, float* h, bool* done) {
  const float t = (millis() - g->flightAt) / 1000.0f * g->flightSpeed;
  const float idx = t / 0.05f;
  const int n = g->shot.points;
  *done = false;
  if (n <= 0) { *x = g->shot.x; *y = g->shot.y; *h = 0; *done = true; return; }
  if (idx >= n - 1) {
    const G::Point3& p = g->traj[n - 1];
    *x = p.x; *y = p.y; *h = p.h < 0 ? 0 : p.h;
    *done = t > (n - 1) * 0.05f + 0.35f;
    return;
  }
  const int i = (int)idx;
  const float f = idx - i;
  const G::Point3& a = g->traj[i];
  const G::Point3& b = g->traj[i + 1];
  *x = a.x + (b.x - a.x) * f;
  *y = a.y + (b.y - a.y) * f;
  *h = a.h + (b.h - a.h) * f;
}

// ---------------- lifecycle ----------------

void freeAll() {
  if (!g) return;
  MI::close();
  heap_caps_free(g->mapBuf);
  heap_caps_free(g->aux);
  heap_caps_free(g->traj);
  heap_caps_free(g->fbCache);
  g->~Game();
  heap_caps_free(g);
  g = nullptr;
}

void enterApp() {
  if (!g) {
    void* mem = heap_caps_calloc(1, sizeof(Game), MALLOC_CAP_SPIRAM);
    if (!mem) { Serial.println("[golf] no PSRAM for the game state"); return; }
    g = new (mem) Game();
    g->mapBuf = (uint16_t*)heap_caps_malloc((size_t)kMapW * kMapMaxH * 2, MALLOC_CAP_SPIRAM);
    g->aux = (uint16_t*)heap_caps_malloc((size_t)kAuxW * kAuxH * 2, MALLOC_CAP_SPIRAM);
    g->traj = (G::Point3*)heap_caps_malloc(sizeof(G::Point3) * kTrajMax, MALLOC_CAP_SPIRAM);
    g->fbCache = (uint16_t*)heap_caps_malloc((size_t)GV::kFeedbackW * GV::kFeedbackH * 2, MALLOC_CAP_SPIRAM);
    if (!g->mapBuf || !g->aux || !g->traj) {
      Serial.println("[golf] no PSRAM for the pictures");
      freeAll();
      return;
    }
    g->tuning = G::defaultTuning();
    g->courseSel = 0;
    g->course = 0;
    g->imu = MI::open();
    if (g->imu) {
      MI::calibrateBegin(millis());
      g->calibrating = true;
    }
    g->activeAt = millis();
    backToTitle();
    Serial.printf("[golf] enter state=%u bytes (PSRAM) + %u + %u + %u; freeInternal=%u freePsram=%u\n",
                  (unsigned)sizeof(Game), (unsigned)(kMapW * kMapMaxH * 2), (unsigned)(kAuxW * kAuxH * 2),
                  (unsigned)(sizeof(G::Point3) * kTrajMax), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
  }
  hal::display::markAllDirty();
}

void resumeApp() {
  if (!g) { enterApp(); return; }
  if (g->mode == Mode::Swing) setMode(Mode::Aim);         // a swing does not survive an overlay
  g->bDown = false;
  hal::display::markAllDirty();
}

void exitApp() {
  services::vibration::release(services::vibration::Owner::Golf);
  freeAll();
  Serial.println("[golf] exit");
}

}  // namespace

void tick() {
  if (!g) return;
  const uint32_t started = micros();
  const uint32_t now = millis();
  ++g->frame;
  if (g->help.tick(now, waiting())) Serial.println("[golf] how to play (held A)");
  if (g->imu) swingTick(now);
  if (g->fbOn) g->fb = G::swingFeedback(g->analyzer);   // live, then frozen at impact (the analyzer holds it)
  runJobs();
  if (g->pendingSwing && g->mode == Mode::Aim) {
    g->pendingSwing = false;
    if (g->swingOnly || g->practiceMode) practiceResult(g->devSwing);
    else hit(g->devSwing);
  }
  if (g->pendingPractice && g->mode == Mode::Aim) {
    g->pendingPractice = false;
    practiceResult(g->devPractice);
  }
  switch (g->mode) {
    case Mode::Flight: {
      float x, y, h;
      bool done;
      flightBall(&x, &y, &h, &done);
      if (done && !g->afterShotHandled) afterFlight();
      break;
    }
    case Mode::Banner:
      if (g->bannerUntil && (int32_t)(now - g->bannerUntil) >= 0) bannerDone();
      break;
    default:
      break;
  }
  g->tickUs = micros() - started;
  hal::display::markAllDirty();
}

namespace {

void fillPlay(GV::Scene& sc, uint32_t now) {
  const bool green = g->auxKind == Aux::Green && (g->mode != Mode::Flight || g->club == G::kPutter) && wantGreenView();
  (void)now;
  sc.screen = GV::Screen::Play;
  sc.map = &g->map;
  sc.theme = g->map.theme();
  sc.pinX = g->map.pinX();
  sc.pinY = g->map.pinY();
  sc.frame = (int)g->frame;
  sc.showBall = true;
  sc.ballX = g->ballX;
  sc.ballY = g->ballY;
  sc.rangeTargets = isRange();
  float targetY = g->ballY + 60;
  const bool live = g->mode == Mode::Swing;
  const bool frozenOn = g->frozenN > 1 && (!g->frozenUntil || (int32_t)(millis() - g->frozenUntil) < 0);
  if (g->mode == Mode::Aim || g->mode == Mode::Swing) {
    sc.showAim = true;
    sc.aimDeg = g->aimDeg;
    // the line: live while swinging (bending with face and path), the last swing's for a while, else straight
    G::SwingResult ls;
    memset(&ls, 0, sizeof(ls));
    ls.quality = 1;
    ls.power = putter() ? fminf(1.0f, toPin() / fmaxf(1.0f, g->putterRange * G::lieFactor(g->lie, g->club))) : 1.0f;
    if (live) {
      // Switch Sports: the line follows the swing from address on - turn = it swings, twist = it bends
      float st, cu;
      g->analyzer.liveShape(&st, &cu);
      const uint32_t t = millis();
      const float dt = g->liveAt ? (float)(uint32_t)(t - g->liveAt) : 0.0f;
      g->liveAt = t;
      g->liveStart = G::smoothToward(g->liveStart, st, dt, 80);
      g->liveCurve = G::smoothToward(g->liveCurve, cu, dt, 80);
      ls.startDeg = g->liveStart;
      ls.curveDeg = g->liveCurve;
      if (g->analyzer.phase() != G::SwingPhase::Address) ls.power = fmaxf(0.15f, fminf(g->analyzer.meterFill(), 1.1f));
    }
    if (!live && frozenOn) {
      sc.preview = g->frozen;
    } else {
      const uint32_t p0 = micros();
      G::previewShot(g->wind, g->club, g->lie, g->ballX, g->ballY, g->aimDeg, ls, g->putterRange, g->livePts);
      g->previewUs = micros() - p0;
      sc.preview = g->livePts;
    }
    sc.previewCount = G::kPreviewPoints;
    const G::SwingResult& shown = (!live && frozenOn) ? g->swing : ls;
    const bool over = (live && g->analyzer.meterRed()) || (!live && frozenOn && g->swing.grade == G::SwingGrade::Over);
    sc.previewWarn = fabsf(shown.startDeg) > 1.5f || fabsf(shown.curveDeg) > 1.5f || over;
    sc.previewColor = shapeColor(shown.startDeg, shown.curveDeg, over);
    // Switch-Sports-style distance dots: 25 / 50 / 75 / 100 % of the club's carry (the same as on the meter)
    const float full = (putter() ? g->putterRange : G::club(g->club).carry) * G::lieFactor(g->lie, g->club);
    sc.distCount = 4;
    for (int k = 0; k < 4; ++k) sc.distYd[k] = full * (k + 1) * 0.25f;
    const G::Point3& e = sc.preview[G::kPreviewPoints - 1];
    const float carry = putter() ? fminf(toPin(), g->putterRange * G::lieFactor(g->lie, g->club))
                                 : G::club(g->club).carry * G::lieFactor(g->lie, g->club);
    sc.aimYd = carry;
    targetY = fmaxf(e.y, g->ballY + 20);
  }
  // The Shot Meter: shown from address (empty, bending with the tilt), frozen with the shot until the next address.
  // It bends like Switch Sports': the way the ball will curve; an over-swing's overshoot whips to its drift side.
  if (g->fbOn && g->mode != Mode::Card && (g->fb.active || g->mode == Mode::Swing)) {
    if (!g->fb.active) {                                   // at address: an empty meter
      memset(&g->fbShown, 0, sizeof(g->fbShown));
      g->fbShown.active = true;
      g->fbShown.phase = 1;
      sc.feedback = &g->fbShown;
    } else {
      sc.feedback = &g->fb;
    }
    if (live || !g->fb.frozen)
      sc.meterBend = G::meterBend(g->liveStart, g->liveCurve, g->fb.fill, g->fb.red, g->liveCurve >= 0 ? 1.0f : -1.0f);
    else
      sc.meterBend = G::meterBend(g->swing.startDeg, g->swing.curveDeg, g->swing.meterPower, g->swing.overswing,
                                  g->swing.driftSide);
    sc.feedbackCarry = g->fbCarry;
    sc.feedbackCache = g->fbCache;
    sc.feedbackKey = &g->fbKey;
  }
  // every hint and status line goes in the bottom strip
  if (g->mode == Mode::Aim && !live && g->practiceMsg[0] && (int32_t)(millis() - g->practiceMsgUntil) < 0)
    sc.hint = g->practiceMsg;
  else if (g->mode == Mode::Aim && g->swingOnly)
    sc.hint = "hold B: swing   C: club   A: title";
  else if (g->mode == Mode::Aim && g->practiceMode)
    sc.hint = "practice mode: hold B to swing   A: back to play";
  if (g->mode == Mode::Flight) {
    float x, y, h;
    bool done;
    flightBall(&x, &y, &h, &done);
    sc.ballX = x; sc.ballY = y; sc.ballH = h;
    targetY = y + (h > 0.5f ? 40 : 20);
    if (frozenOn) {                                      // the line it was meant to fly, faintly
      sc.preview = g->frozen;
      sc.previewCount = g->frozenN;
      sc.previewAlpha = 110;
      sc.previewWarn = fabsf(g->swing.startDeg) > 1.5f || fabsf(g->swing.curveDeg) > 1.5f || g->swing.overswing;
      sc.previewColor = shapeColor(g->swing.startDeg, g->swing.curveDeg, g->swing.overswing);
    }
  }
  if (green) {
    sc.image = g->aux; sc.imageW = g->auxW; sc.imageH = g->auxH; sc.view = g->auxView;
    sc.camX = 0; sc.camY = 0;
    sc.bigFlag = true;
    sc.showSlope = true;
    sc.slopeX = g->map.slopeX();
    sc.slopeY = g->map.slopeY();
  } else {
    updateCamera(targetY, false);
    sc.image = g->mapBuf; sc.imageW = kMapW; sc.imageH = g->mapView.h; sc.view = g->mapView;
    sc.camX = 0; sc.camY = (int)g->camY;
  }
  sc.hud = g->mode == Mode::Aim || g->mode == Mode::Swing;
  sc.hole = g->hole + 1;
  sc.par = holeDef().par;
  sc.yards = (int)lroundf(g->map.yards());
  sc.stroke = g->stroke + 1;
  sc.toPin = toPin();
  sc.club = g->club;
  sc.clubYd = (putter() ? g->putterRange : G::club(g->club).carry) * G::lieFactor(g->lie, g->club);
  sc.lieName = G::surfaceName(g->lie);
  sc.windMph = g->wind.mph;
  sc.windDeg = g->wind.dirDeg;
  if (isRange()) { sc.hole = 0; }
  if (g->swingOnly) sc.title = "Swing practice";
}

}  // namespace

static uint32_t micros32() { return (uint32_t)micros(); }

void onDraw(TFT_eSprite& canvas) {
  const uint32_t started = micros();
  setSceneChromeBackground(GV::gfx::rgb(20, 40, 24));
  uint16_t* px = (uint16_t*)canvas.getPointer();
  if (!g) {
    canvas.fillSprite(TFT_BLACK);
    canvas.setTextColor(TFT_WHITE);
    canvas.drawString("Golf needs PSRAM", 60, 120);
    return;
  }
  const uint32_t now = millis();
  GV::Scene& sc = g->scene;
  GV::clearScene(sc);
  sc.frame = (int)g->frame;
  switch (g->mode) {
    case Mode::Title:
    case Mode::CourseMenu: {
      sc.screen = g->mode == Mode::Title ? GV::Screen::Title : GV::Screen::CourseMenu;
      if (g->auxKind == Aux::Title) { sc.image = g->aux; sc.imageW = g->auxW; sc.imageH = g->auxH; sc.view = g->auxView; }
      if (g->mode == Mode::Title) {
        sc.title = "motion golf";
        sc.items = kCourseItems; sc.itemCount = kTitleItems; sc.selected = g->courseSel;
      } else {
        sc.title = kCourseItems[g->courseSel];
        sc.items = kHoleItems; sc.itemCount = 10; sc.selected = g->menuSel;
      }
      if (!g->imu) sc.hint = "no motion sensor";
      else if (g->calibrating && g->mode == Mode::Title) sc.hint = nullptr;
      else sc.hint = g->mode == Mode::Title ? "dial: choose  C: select  B: recalibrate" : "dial: choose  C: start  A: back";
      break;
    }
    case Mode::Intro:
    case Mode::Overview: {
      sc.screen = GV::Screen::Intro;
      sc.progress = g->job == Job::Map ? g->renderer.percent() : 100;
      if (g->auxKind == Aux::Overview) { sc.image = g->aux; sc.imageW = g->auxW; sc.imageH = g->auxH; sc.view = g->auxView; }
      else sc.progress = sc.progress >= 100 ? 99 : sc.progress;
      sc.courseName = G::course(g->course).name;
      sc.hole = g->hole + 1; sc.par = holeDef().par; sc.yards = (int)lroundf(g->map.yards());
      sc.windMph = g->wind.mph; sc.windDeg = g->wind.dirDeg;
      sc.pinX = g->map.pinX(); sc.pinY = g->map.pinY();
      sc.showBall = true; sc.ballX = g->ballX; sc.ballY = g->ballY;
      sc.stroke = g->mode == Mode::Overview ? 2 : 1;
      if (isRange()) sc.hole = 0;
      break;
    }
    case Mode::Card: {
      uint8_t* pars = g->cardPars;
      const G::CourseDef& c = G::course(g->course);
      for (int i = 0; i < 9; ++i) pars[i] = i < c.holeCount ? c.holes[i].par : 0;
      fillPlay(sc, now);
      sc.screen = GV::Screen::Card;
      sc.hud = false;
      sc.title = c.name;
      sc.pars = pars;
      sc.strokes = g->strokes;
      sc.holeCount = c.holeCount;
      if (g->practice) {
        memset(g->cardOne, 0, sizeof(g->cardOne));
        g->cardOne[g->hole] = (uint8_t)g->stroke;
        sc.strokes = g->cardOne;
      }
      sc.hint = g->practice ? "C: play it again   A: menu" : (g->hole + 1 < c.holeCount ? "C: next hole" : "C: finish");
      break;
    }
    default:
      fillPlay(sc, now);
      if (g->mode == Mode::Banner) {
        if (g->swingOnly && g->frozenN > 1) {
          sc.preview = g->frozen;
          sc.previewCount = g->frozenN;
          sc.previewWarn = fabsf(g->swing.startDeg) > 1.5f || fabsf(g->swing.curveDeg) > 1.5f || g->swing.overswing;
          sc.previewColor = shapeColor(g->swing.startDeg, g->swing.curveDeg, g->swing.overswing);
        }
        if (g->swingOnly) sc.hint = "B: swing again   C: club   A: title";
        sc.banner = g->banner[0] ? g->banner : nullptr;       // an empty banner: just the strip line
        sc.sub = g->sub[0] ? g->sub : nullptr;
        sc.bannerColor = g->bannerColor;
        for (int i = 0; i < 4; ++i) sc.lines[i] = g->lines[i][0] ? g->lines[i] : nullptr;
        if (!g->bannerUntil && !g->lines[0][0]) sc.hint = "C: continue";
        if (!g->banner[0] && g->practiceMsg[0]) sc.hint = g->practiceMsg;
      }
      if (g->job == Job::Green && g->mode == Mode::Aim) sc.hint = "loading the green...";
      break;
  }
  sc.helpHint = waiting();
  sc.helpOpen = g->help.isOpen();
  sc.helpPage = g->help.page();
  g->timing.clock = micros32;
  GV::drawScene(px, sc, &g->timing);
  g->drawUs = micros() - started;
  if (g->drawUs > g->maxDrawUs) g->maxDrawUs = g->drawUs;
  hal::display::markAllDirty();
}

void onInput(hal::input::InputEvent ev) {
  if (!g) return;
  using ET = hal::input::EventType;
  g->activeAt = millis();
  {  // hold A: how to play (it takes the inputs while open; a long A never reaches the game)
    using HE = GV::HelpEvent;
    HE he = HE::Other;
    switch (ev.type) {
      case ET::ButtonA_Press: he = HE::APress; break;
      case ET::ButtonA_Release: he = HE::ARelease; break;
      case ET::ButtonA_Tap: he = HE::ATap; break;
      case ET::ButtonB_Press: he = HE::BPress; break;
      case ET::ButtonB_Release: he = HE::BRelease; break;
      case ET::ButtonB_Tap: he = HE::BTap; break;
      case ET::ButtonC_Press: he = HE::CPress; break;
      case ET::ButtonC_Release: he = HE::CRelease; break;
      case ET::ButtonC_Tap: he = HE::CTap; break;
      case ET::Scroll: he = HE::Scroll; break;
      case ET::Chord_AB: he = HE::ChordAB; break;
      default: break;
    }
    const GV::HelpAction a = g->help.event(he, millis(), waiting(), ev.delta);
    if (a == GV::HelpAction::Opened) Serial.println("[golf] how to play");
    if (a != GV::HelpAction::Pass) {
      hal::display::markAllDirty();
      return;
    }
  }
  switch (g->mode) {
    case Mode::Title:
      if (ev.type == ET::Scroll && ev.delta) {
        g->courseSel = (int8_t)((g->courseSel + (ev.delta > 0 ? 1 : kTitleItems - 1)) % kTitleItems);
        titleBackdrop();
      } else if (ev.type == ET::ButtonC_Tap) {
        if (g->courseSel == 4) startPractice();
        else if (g->courseSel == 3) startRound(3, 0, true);
        else { g->menuSel = 0; setMode(Mode::CourseMenu); }
      } else if (ev.type == ET::ButtonB_Tap && g->imu) {
        MI::calibrateBegin(millis());
        g->calibrating = true;
      }
      return;
    case Mode::CourseMenu:
      if (ev.type == ET::Scroll && ev.delta) {
        g->menuSel = (int8_t)((g->menuSel + (ev.delta > 0 ? 1 : 9)) % 10);
      } else if (ev.type == ET::ButtonC_Tap) {
        if (g->menuSel == 0) startRound(g->courseSel, 0, false);
        else startRound(g->courseSel, g->menuSel - 1, true);
      } else if (ev.type == ET::ButtonA_Tap) {
        setMode(Mode::Title);
      }
      return;
    case Mode::Intro:
      if ((ev.type == ET::ButtonC_Tap || ev.type == ET::ButtonB_Tap) && g->job != Job::Map) {
        g->camY = 1e9f;
        setMode(Mode::Aim);
        if (wantGreenView() && g->auxKind != Aux::Green) startJob(Job::Green);
      } else if (ev.type == ET::ButtonA_Tap) {
        backToTitle();
      }
      return;
    case Mode::Overview:
      if (ev.type == ET::ButtonC_Tap || ev.type == ET::ButtonB_Press || ev.type == ET::ButtonA_Tap) {
        g->tapOk = false;
        g->camY = 1e9f;
        setMode(Mode::Aim);
        if (wantGreenView()) startJob(Job::Green);
      }
      return;
    case Mode::Aim:
      if (ev.type == ET::Scroll || ev.type == ET::ButtonA_Tap || ev.type == ET::ButtonC_Tap) g->frozenN = 0;  // stale
      if (ev.type == ET::Scroll && ev.delta) {
        g->aimDeg += ev.delta * (putter() ? 0.5f : 1.0f);
        if (g->aimDeg > 180) g->aimDeg -= 360;
        if (g->aimDeg < -180) g->aimDeg += 360;
      } else if (ev.type == ET::ButtonC_Tap) {
        g->club = (g->club + 1) % G::kClubCount;
        if (putter()) g->putterRange = G::putterRangeFor(toPin());
      } else if (ev.type == ET::ButtonA_Tap) {               // A: practice mode on / off (the dial press changes club)
        if (g->swingOnly) { backToTitle(); return; }
        g->practiceMode = !g->practiceMode;
        clearFeedback();
        g->practiceMsg[0] = 0;
        g->frozenN = 0;
        buzz(20);
        Serial.printf("[golf] practice mode %s\n", g->practiceMode ? "on" : "off");
      } else if (ev.type == ET::ButtonB_Press) {
        g->bDown = true;
        g->bDownAt = millis();
        g->tapOk = false;
        if (g->imu && g->job != Job::Green) {               // B: this pose is neutral (bias: the live estimate)
          address(true);
          g->liveStart = g->liveCurve = 0;                  // the live line starts straight
          g->liveAt = 0;
          g->fbOn = true;                                   // the bars follow this swing (hidden until the backswing)
          g->following = false;
          g->fbCarry = (putter() ? g->putterRange : G::club(g->club).carry) * G::lieFactor(g->lie, g->club);
          setMode(Mode::Swing);
        }
      } else if (ev.type == ET::ButtonB_Release && g->bDown) {
        g->bDown = false;
        g->tapOk = (int32_t)(millis() - g->bDownAt) < 350;
      } else if (ev.type == ET::ButtonB_Tap) {                // a quick tap at address: the whole hole
        if (g->tapOk && g->job != Job::Green && g->job != Job::Map) {
          makeOverview();
          setMode(Mode::Overview);
        }
        g->tapOk = false;
      }
      return;
    case Mode::Swing:
      if (ev.type == ET::ButtonB_Release) {
        swingTick(millis());                                // an impact already in the FIFO still counts
        g->bDown = false;
        if (g->mode != Mode::Swing) return;                 // it hit: the release (and its tap) belong to the shot
        g->tapOk = (int32_t)(millis() - g->bDownAt) < 350 && g->analyzer.phase() == G::SwingPhase::Address;
        // A swing that didn't hit: log why, to tune the thresholds from real swings.
        Serial.printf("[golf] no stroke: phase %u peak %.0f dps (a stroke needs %.0f, full %.0f) held %u ms\n",
                      (unsigned)g->analyzer.phase(), g->analyzer.peakDps(), g->analyzer.countDps(),
                      g->analyzer.fullDps(), (unsigned)(millis() - g->bDownAt));
        clearFeedback();
        setMode(Mode::Aim);                                 // released before impact: cancelled
      }
      return;
    case Mode::Banner:
      if (g->swingOnly && ev.type == ET::ButtonB_Press && !g->bannerUntil) {   // Swing Practice: B swings again
        g->lines[0][0] = g->lines[1][0] = g->lines[2][0] = g->lines[3][0] = 0;
        setMode(Mode::Aim);
        onInput(ev);
        return;
      }
      if (ev.type == ET::ButtonB_Press) { g->bDown = true; g->bDownAt = millis(); g->tapOk = true; return; }
      if (ev.type == ET::ButtonB_Release) { g->bDown = false; return; }
      // B only counts if it was pressed during the banner (not the swing's own release)
      if (ev.type == ET::ButtonB_Tap && !(g->tapOk && (int32_t)(g->bDownAt - g->modeAt) > 0)) return;
      if ((ev.type == ET::ButtonC_Tap || ev.type == ET::ButtonB_Tap) && (!g->bannerUntil || (int32_t)(millis() - g->modeAt) > 400)) {
        g->tapOk = false;
        bannerDone();
      }
      return;
    case Mode::Card:
      if (ev.type == ET::ButtonC_Tap) cardDone();
      else if (ev.type == ET::ButtonA_Tap && g->practice) backToTitle();
      return;
    default:
      return;
  }
}

#ifdef ORIGAMI_DEV_SERIAL_INPUT
void devCommand(const char* args, Print& out) {
  while (*args == ' ') ++args;
  if (!g) { out.println("[golf] not open"); return; }
  if (strncmp(args, "hole", 4) == 0) {
    int c = 1, h = 1;
    sscanf(args + 4, "%d %d", &c, &h);
    c = c < 1 ? 1 : (c > 4 ? 4 : c);
    const int n = G::course(c - 1).holeCount;
    h = h < 1 ? 1 : (h > n ? n : h);
    startRound(c - 1, h - 1, c == 4);
    out.printf("[golf] hole %d of %s\n", h, G::course(c - 1).name);
  } else if (strncmp(args, "swing", 5) == 0) {
    float dps = 450, yaw = 0, twist = 0;                  // golf swing <peakDps> <pathDeg> <tiltDeg>
    sscanf(args + 5, "%f %f %f", &dps, &yaw, &twist);
    g->devSwing = G::synthesizeSwing(dps, yaw, twist, g->club, g->tuning);
    g->pendingSwing = true;
    out.printf("[golf] swing queued: power %.2f speed %.2f perfect %d start %.1f curve %.1f (plays in Aim)\n",
               g->devSwing.power, g->devSwing.speed, g->devSwing.perfect, g->devSwing.startDeg, g->devSwing.curveDeg);
  } else if (strncmp(args, "help", 4) == 0) {
    if (strstr(args, "close")) g->help.close();
    else if (strstr(args, "next")) g->help.event(GV::HelpEvent::Scroll, millis(), true, 1);
    else g->help.open();
    out.printf("[golf] how to play %s, page %d of %d\n", g->help.isOpen() ? "open" : "closed", g->help.page() + 1,
               GV::helpPageCount());
  } else if (strncmp(args, "practice", 8) == 0) {
    startPractice();
    out.println("[golf] Swing Practice (the range; swings are shown, nothing is hit)");
  } else if (strncmp(args, "trace", 5) == 0) {
    g->trace = !g->trace;
    out.printf("[golf] trace %s (bias %.2f %.2f %.2f)\n", g->trace ? "on" : "off", MI::bias()[0], MI::bias()[1],
               MI::bias()[2]);
  } else if (strncmp(args, "pswing", 6) == 0) {
    float dps = 450, yaw = 0, twist = 0;
    sscanf(args + 6, "%f %f %f", &dps, &yaw, &twist);
    g->devPractice = G::synthesizeSwing(dps, yaw, twist, g->club, g->tuning);
    g->pendingPractice = true;
    out.printf("[golf] practice swing queued: meter %.2f grade %u shape %s (shows in Aim)\n", g->devPractice.meterPower,
               (unsigned)g->devPractice.grade, G::shotShape(g->devPractice));
  } else if (strncmp(args, "auto", 4) == 0) {
    const G::Plan p = G::caddie(g->map, g->wind, g->ballX, g->ballY, g->lie, true);
    g->club = p.club;
    g->aimDeg = p.aimDeg;
    g->putterRange = p.putterRange;
    G::SwingResult r;
    memset(&r, 0, sizeof(r));
    r.power = p.power;
    r.quality = 1;
    r.speed = 1.2f;
    r.perfect = true;
    g->devSwing = r;
    g->pendingSwing = true;
    out.printf("[golf] auto: %s aim %.1f power %.2f\n", G::club(p.club).name, p.aimDeg, p.power);
  } else if (strncmp(args, "club", 4) == 0) {
    g->club = atoi(args + 4) % G::kClubCount;
    if (putter()) g->putterRange = G::putterRangeFor(toPin());
    out.printf("[golf] club %s\n", G::club(g->club).name);
  } else if (strncmp(args, "aim", 3) == 0) {
    g->aimDeg = (float)atof(args + 3);
    out.printf("[golf] aim %.1f\n", g->aimDeg);
  } else if (strncmp(args, "wind", 4) == 0) {
    sscanf(args + 4, "%f %f", &g->wind.mph, &g->wind.dirDeg);
    out.printf("[golf] wind %.0f mph toward %.0f\n", g->wind.mph, g->wind.dirDeg);
  } else if (strncmp(args, "flip", 4) == 0) {
    if (strstr(args, "yaw")) g->tuning.yawSign = -g->tuning.yawSign;
    if (strstr(args, "twist")) g->tuning.twistSign = -g->tuning.twistSign;
    out.printf("[golf] yawSign %.0f twistSign %.0f\n", g->tuning.yawSign, g->tuning.twistSign);
  } else if (strncmp(args, "next", 4) == 0) {
    hal::input::InputEvent ev;
    ev.type = hal::input::EventType::ButtonC_Tap;
    onInput(ev);
    out.println("[golf] next");
  } else if (strncmp(args, "menu", 4) == 0) {
    backToTitle();
    out.println("[golf] menu");
  } else {
    out.printf("[golf] mode=%u course=%d hole=%d stroke=%d ball=%.1f,%.1f lie=%s club=%s aim=%.1f toPin=%.1f wind=%.0f@%.0f "
               "job=%u aux=%u cal=%d/%d draw=%uus (bg %u world %u hud %u) max=%uus preview=%uus tick=%uus freeInternal=%u freePsram=%u\n",
               (unsigned)g->mode, g->course + 1, g->hole + 1, g->stroke, g->ballX, g->ballY, G::surfaceName(g->lie),
               G::club(g->club).shortName, g->aimDeg, toPin(), g->wind.mph, g->wind.dirDeg, (unsigned)g->job,
               (unsigned)g->auxKind, g->calibrated, g->calibrating, (unsigned)g->drawUs, (unsigned)g->timing.bgUs,
               (unsigned)g->timing.worldUs, (unsigned)g->timing.hudUs, (unsigned)g->maxDrawUs, (unsigned)g->previewUs,
               (unsigned)g->tickUs,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL), (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    g->maxDrawUs = 0;                                     // max is since the last status
    if (g->mode == Mode::Swing)
      out.printf("[golf] swing phase=%u live=%.2f swing=%.1f\n", (unsigned)g->analyzer.phase(), g->analyzer.meterFill(),
                 g->lastSwing);
  }
}
#endif

namespace {

void screenTick(uint32_t) { tick(); }

// You play by swinging, so the system's 60 s idle return-to-home would fire mid-round: stay busy while there was
// a swing or a button in the last minute (and while a hole is being drawn).
core::health::RuntimePolicy runtimePolicy() {
  const bool busy = g && ((int32_t)(millis() - g->activeAt) < 60000 || g->job != Job::None || g->help.isOpen());
  return core::health::whileBusy(busy);
}

void buildGolfScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  scene.addRect("golf.bg", {0, 0, width, height}, GV::gfx::rgb(20, 40, 24), 0, 0);
}

const ::scene::Screen kScreenGolf = {
    "Golf", nullptr, nullptr, screenTick, onInput, nullptr, onDraw, "game", buildGolfScene,
};
const ::scene::Screen* const kScreens[] = {&kScreenGolf};

}  // namespace

App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Golf", "golf", &kScreenGolf, enterApp, resumeApp, exitApp,
                                            "src/apps/golf_app.cpp", kScreens, nullptr, runtimePolicy);

}  // namespace golf
}  // namespace apps

#endif  // MENU_APP
