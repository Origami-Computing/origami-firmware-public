/**
 * @file bowling_app.cpp
 * @brief Wii-Sports-style motion bowling: hold B, swing the kami like the ball, let go.
 *
 * Enter -> aim (no calibration step: the gyro offset is estimated live): A/B tap step left/right, the dial turns the
 * aim line. Hold B: the pose at
 * that moment is the address; swing back, then forward, and release B (or keep holding: it lets go at the end of
 * the forward swing). Swing speed = ball speed, the swing's heading = its line, wrist twist = hook. C (dial click)
 * starts a new game at the end. Logic is core/bowling (host-tested), drawing is
 * games/bowling_view; this file reads the IMU and the buttons, steps the physics, and times the screens.
 */

#if defined(MENU_APP) && defined(KAMI_GAMES) && KAMI_GAMES  // N16R8 only (platformio.ini)

#include "apps/bowling_app.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <math.h>
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/bowling.h"
#include "core/health_policy.h"
#include "games/bowling_help.h"
#include "games/bowling_view.h"
#include "services/sports_motion.h"
#include "hal/hal_display.h"
#include "services/vibration.h"
#include "scene/screen.h"
#include "ui/scene.h"
#include "ui/screen_chrome.h"

namespace apps {
namespace bowling {

namespace B = core::bowling;
namespace V = games::bowling_view;
namespace MI = services::motion_input;
namespace H = games::bowling_help;

enum class Mode : uint8_t { NoImu, Play };

// The match and the swing reader live in PSRAM while the app is open.
struct Game {
  B::Match match;
  B::SwingReader reader;
  V::Retained view;        // what the last frame drew: only changes are repainted (~9 KB)
  H::Help help;            // hold A: how to play
};

struct State {
  Mode mode;
  bool holding;            // B is down and the swing is being read
  bool thrown;             // this ball has left the hand (follow-through pose)
  V::View view;
  uint32_t pressMs, releaseMs, resultAt, msgUntil, activeAt, lastTickMs, statsAt;
  float relArm, speedMph;
  int best;
  uint32_t frames, drawUs, maxDrawUs;
  uint32_t sumMeasure, sumBg, sumScene, sumHud, sumArea, sumBlends;   // over the stats window
  char msg[32];
};

static State s;
static Game* g = nullptr;

static constexpr uint16_t kChrome = (uint16_t)(((12 >> 3) << 11) | ((16 >> 2) << 5) | (30 >> 3));
static constexpr float kDeckCutY = 14.5f;         // the ball passes this: cut to the pin deck
static constexpr uint32_t kResultMs = 2600;       // how long the banner stays before the next ball
static constexpr int kStepBoards = 2;

static void say(const char* text, uint32_t ms = 1400) {
  snprintf(s.msg, sizeof(s.msg), "%s", text);
  s.msgUntil = millis() + ms;
}

static void buzz(uint32_t ms) {
  services::vibration::pulse(services::vibration::Owner::Bowling, ms);
}

// No "hold still" step (2026-09-30, the user): pressing B takes the neutral pose and the live gyro offset
// (core/bias_estimator via MI::bias(), refined whenever the kami rests), so you can play straight away.
static void beginPlay() {
  s.holding = false;
  s.mode = MI::isOpen() ? Mode::Play : Mode::NoImu;
}

static void launch(const B::Throw& t) {
  if (!g || !g->match.throwBall(t)) return;
  const uint32_t now = millis();
  s.thrown = true;
  s.releaseMs = now;
  s.speedMph = t.speed * 2.237f;
  s.view = V::View::Approach;
  s.activeAt = now;
  buzz(35);
  Serial.printf("[bowling] throw speed=%.2f m/s angle=%.2f spin=%.2f x=%.3f\n", t.speed, t.angleDeg, t.spin, t.startX);
}

static void throwFromSwing() {
  const B::SwingReading& r = g->reader.reading();
  const B::Throw t = B::makeThrow(r, g->match.aimDeg(), g->match.startX());
  Serial.printf("[bowling] swing peak=%.0f dps heading=%.1f twist=%.1f back=%.0f\n", r.peakDps, r.headingDeg,
                r.twistDeg, r.backswingDeg);
  s.relArm = s.relArm < -40 ? s.relArm : -40;
  launch(t);
}

// Feed the swing reader every gyro sample since the press; it may let go by itself at the end of the swing.
static void feed(int n) {
  if (!s.holding) return;
  const MI::Sample* sm = MI::samples();
  for (int i = 0; i < n; ++i) {
    if ((int32_t)(sm[i].ms - s.pressMs) < 0) continue;
    if (g->reader.sample(sm[i].ms, sm[i].dps) == B::SwingResult::Throw) {
      s.holding = false;
      throwFromSwing();
      return;
    }
  }
  s.relArm = g->reader.armDeg() - 40;                // the arm hangs 40 degrees forward at address
}

static void enter() {
  memset(&s, 0, sizeof(s));
  s.activeAt = millis();
  s.view = V::View::Approach;
  if (!g) {
    void* mem = heap_caps_malloc(sizeof(Game), MALLOC_CAP_SPIRAM);
    if (mem) g = new (mem) Game();
  }
  if (g) {
    g->match.reset();
    V::invalidate(g->view);
  }
  if (g && MI::open()) beginPlay();
  else s.mode = Mode::NoImu;
  Serial.printf("[bowling] enter game=%u B (PSRAM) state=%u B imu=%d\n", (unsigned)sizeof(Game), (unsigned)sizeof(s),
                s.mode != Mode::NoImu);
  hal::display::markAllDirty();
}

// Back from an overlay or alert: keep the game (the next B press re-takes the neutral pose).
static void resume() {
  if (!g) { enter(); return; }
  s.holding = false;
  s.activeAt = millis();
  V::invalidate(g->view);                            // an overlay may have drawn over the canvas
  if (!MI::isOpen()) MI::open();
  beginPlay();
  hal::display::markAllDirty();
}

static void exitApp() {
  MI::close();
  if (g) {
    g->~Game();
    heap_caps_free(g);
    g = nullptr;
  }
  services::vibration::release(services::vibration::Owner::Bowling);
  Serial.println("[bowling] exit");
}

static void stepPhysics(uint32_t now, float dt) {
  B::Match& m = g->match;
  if (m.phase() == B::Phase::Rolling) {
    s.activeAt = now;                                // watching the ball is activity too
    const B::Lane& L = m.lane();
    if (s.view == V::View::Approach && L.ball().y > kDeckCutY) s.view = V::View::Deck;
    // At ~12 fps the ball would cross the deck in two frames: slow motion in the close-up, slowest at impact.
    float scale = 1.0f;
    if (s.view == V::View::Deck) {
      const float since = L.sinceHit();
      scale = since < 0 ? 0.6f : (since < 0.5f ? 0.3f : 0.7f);
    }
    if (m.advance(dt * scale)) {
      s.resultAt = now;
      s.view = V::View::Deck;
      const B::Outcome o = m.outcome();
      if (o == B::Outcome::Strike) buzz(160);
      else if (o == B::Outcome::Spare) buzz(90);
      Serial.printf("[bowling] result outcome=%d count=%d standing=%03x frame=%d score=%d\n", (int)o, m.lastCount(),
                    L.standing(), m.card().frame() + 1, m.card().score());
    }
  } else if (m.phase() == B::Phase::Result && (int32_t)(now - s.resultAt) >= (int32_t)kResultMs) {
    m.next();
    s.thrown = false;
    s.speedMph = 0;
    s.resultAt = 0;
    s.view = V::View::Approach;
    s.activeAt = now;
    if (m.phase() == B::Phase::GameOver) {
      if (m.card().score() > s.best) s.best = m.card().score();
      Serial.printf("[bowling] game over score=%d best=%d\n", m.card().score(), s.best);
    }
  }
}

// The how-to-play panel opens only while the game waits for the player, never mid-swing or while the ball rolls.
static bool waitingForPlayer() {
  if (!g || s.holding) return false;
  const B::Phase p = g->match.phase();
  return p == B::Phase::Aiming || p == B::Phase::Result || p == B::Phase::GameOver;
}

// Buttons go to the panel first (see games/bowling_help.h); true = it took the event.
static bool helpTakes(hal::input::InputEvent ev, uint32_t now) {
  using ET = hal::input::EventType;
  H::Ev e;
  switch (ev.type) {
    case ET::ButtonA_Press: e = H::Ev::APress; break;
    case ET::ButtonA_Release: e = H::Ev::ARelease; break;
    case ET::ButtonA_Tap: e = H::Ev::ATap; break;
    case ET::ButtonB_Press: e = H::Ev::BPress; break;
    case ET::ButtonB_Release: e = H::Ev::BRelease; break;
    case ET::ButtonB_Tap: e = H::Ev::BTap; break;
    case ET::ButtonC_Press: e = H::Ev::CPress; break;
    case ET::ButtonC_Release: e = H::Ev::CRelease; break;
    case ET::ButtonC_Tap: e = H::Ev::CTap; break;
    case ET::Scroll: e = H::Ev::Scroll; break;
    case ET::Chord_AB: e = H::Ev::Chord; break;
    default: return false;
  }
  const H::Act a = g->help.input(e, now, waitingForPlayer(), ev.delta);
  if (a == H::Act::Opened) Serial.println("[bowling] help open");
  if (a == H::Act::Closed) Serial.println("[bowling] help closed");
  return a != H::Act::Pass;
}

void tick() {
  const uint32_t now = millis();
  const float dt = s.lastTickMs ? fminf(0.1f, (now - s.lastTickMs) / 1000.0f) : 0.0f;
  s.lastTickMs = now;
  if (!g) return;
  const int n = MI::isOpen() ? MI::poll(now) : 0;
  if (g->help.tick(now, waitingForPlayer()) == H::Act::Opened) Serial.println("[bowling] help open");
  if (g->help.isOpen()) s.activeAt = now;               // reading the panel is activity
  if (s.mode == Mode::Play) {
    feed(n);
  }
  stepPhysics(now, dt);
  if ((int32_t)(now - s.statsAt) >= 5000) {
    const uint32_t nf = s.frames ? s.frames : 1;
    Serial.printf("[bowling] fps=%.1f draw avg=%uus (measure %u bg %u scene %u hud %u; repaint %u px, painted %u px) "
                  "max=%uus freeInternal=%u largest=%u psram=%u\n", s.frames * 1000.0f / (now - s.statsAt + 1),
                  (unsigned)(s.drawUs / nf), (unsigned)(s.sumMeasure / nf), (unsigned)(s.sumBg / nf),
                  (unsigned)(s.sumScene / nf), (unsigned)(s.sumHud / nf), (unsigned)(s.sumArea / nf),
                  (unsigned)(s.sumBlends / nf), (unsigned)s.maxDrawUs,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    s.frames = 0;
    s.maxDrawUs = 0;
    s.drawUs = s.sumMeasure = s.sumBg = s.sumScene = s.sumHud = s.sumArea = s.sumBlends = 0;
    s.statsAt = now;
  }
}

static const char* hintFor(uint32_t now) {
  if (s.mode == Mode::NoImu) return "no motion sensor";
  static const char* const kHelpHint = "hold A: how to play";
  if (g->match.phase() == B::Phase::Rolling) return nullptr;
  if (g->match.phase() != B::Phase::Aiming) return kHelpHint;
  if (s.holding) {
    switch (g->reader.stage()) {
      case B::SwingStage::Address: return "swing back...";
      case B::SwingStage::Backswing: return "...and through! let go";
      default: return "let go of B";
    }
  }
  switch ((now / 3000) % 3) {
    case 0: return "hold B, swing, let go";
    case 1: return "A/B step, dial aims";
    default: return kHelpHint;
  }
}

void onDraw(TFT_eSprite& canvas) {
  const uint32_t started = micros();
  const uint32_t now = millis();
  setSceneChromeBackground(kChrome);
  if (!g) {
    canvas.fillRect(0, 38, canvas.width(), canvas.height() - 38, kChrome);
    return;
  }
  const B::Match& m = g->match;
  V::Scene sc;
  memset(&sc, 0, sizeof(sc));
  sc.view = s.view;
  sc.nowMs = now;
  sc.best = s.best;
  if (s.thrown) {
    sc.pose = V::Pose::FollowThrough;
    const float t = fminf(1.0f, (now - s.releaseMs) / 260.0f);
    sc.armDeg = s.relArm + (-135.0f - s.relArm) * t;
    sc.ballInHand = false;
  } else if (s.holding) {
    sc.pose = V::Pose::Swing;
    sc.armDeg = fmaxf(-150.0f, fminf(110.0f, s.relArm));
    sc.ballInHand = true;
  } else {
    sc.pose = V::Pose::Stance;
    sc.ballInHand = true;
  }
  sc.resultMs = s.resultAt && m.phase() == B::Phase::Result ? (now - s.resultAt) | 1u : 0;
  sc.speedMph = s.speedMph;
  sc.hint = hintFor(now);
  sc.helpPage = (int8_t)(g->help.isOpen() ? g->help.page() + 1 : 0);
  if ((int32_t)(now - s.msgUntil) < 0) sc.center = s.msg;
  V::Timing tm;
  memset(&tm, 0, sizeof(tm));
  tm.clockUs = []() -> uint32_t { return (uint32_t)micros(); };
  V::draw((uint16_t*)canvas.getPointer(), m, sc, &g->view, &tm);
  const uint32_t us = micros() - started;
  s.drawUs += us;
  if (us > s.maxDrawUs) s.maxDrawUs = us;
  s.sumMeasure += tm.measureUs;
  s.sumBg += tm.bgUs;
  s.sumScene += tm.sceneUs;
  s.sumHud += tm.hudUs;
  s.sumArea += tm.area;
  s.sumBlends += tm.blends;
  ++s.frames;
  // Only the rows that changed go to the display (none when nothing moved).
  for (int k = 0; k < g->view.rects; ++k) hal::display::markDirty(g->view.rect[k].y0, g->view.rect[k].y1 - g->view.rect[k].y0);
}

void onInput(hal::input::InputEvent ev) {
  using ET = hal::input::EventType;
  const uint32_t now = millis();
  s.activeAt = now;
  if (!g) return;
  if (helpTakes(ev, now)) return;                      // the panel (or the hold that opens it) owns this event
  B::Match& m = g->match;
  if (ev.type == ET::ButtonC_Tap) {
    if (m.phase() == B::Phase::GameOver) {
      m.reset();
      s.thrown = false;
      s.view = V::View::Approach;
      say("new game", 900);
    }
    return;
  }
  if (m.phase() != B::Phase::Aiming) {
    if (ev.type == ET::ButtonB_Release) s.holding = false;
    return;
  }
  switch (ev.type) {
    case ET::Scroll:
      if (!s.holding) m.aim(ev.delta);
      break;
    case ET::ButtonA_Tap:
      if (!s.holding) m.move(-kStepBoards);
      break;
    case ET::ButtonB_Press: {
      if (s.mode != Mode::Play) break;
      MI::poll(now);                                   // older samples are not part of this swing
      float acc[3];
      if (!MI::readAccel(acc)) memcpy(acc, MI::upMg(), sizeof(acc));
      g->reader.setBias(MI::bias());                  // the live per-device offset (it keeps improving while resting)
      g->reader.press(now, acc);
      s.holding = true;
      s.pressMs = now;
      s.relArm = -40;
      break;
    }
    case ET::ButtonB_Release: {
      if (s.mode == Mode::NoImu) { m.move(kStepBoards); break; }
      if (!s.holding) break;
      feed(MI::poll(now));
      if (!s.holding) break;                           // it let go by itself
      s.holding = false;
      const unsigned stageBefore = (unsigned)g->reader.stage();   // release() resets it
      switch (g->reader.release(now)) {
        case B::SwingResult::Throw: throwFromSwing(); break;
        case B::SwingResult::Tap: m.move(kStepBoards); break;
        case B::SwingResult::Cancel:
          say("swing back, then through");
          Serial.printf("[bowling] no throw: stage %u backswing %.0f deg (needs 20) forward peak %.0f dps\n",
                        stageBefore, g->reader.backswingSoFar(), g->reader.peakSoFar());
          break;
        default: break;
      }
      break;
    }
    default:
      break;
  }
}

#ifdef ORIGAMI_DEV_SERIAL_INPUT
void devCommand(const char* args, Print& out) {
  while (*args == ' ') ++args;
  if (!g) { out.println("[bowl] not running: open Games -> Bowling"); return; }
  B::Match& m = g->match;
  s.activeAt = millis();
  if (strncmp(args, "throw", 5) == 0) {
    char* p = (char*)args + 5;
    const float speed = strtof(p, &p);
    const float heading = strtof(p, &p);
    const float twist = strtof(p, &p);
    if (m.phase() != B::Phase::Aiming) { out.printf("[bowl] not aiming (phase %d)\n", (int)m.phase()); return; }
    const float v = speed > 0 ? speed : 8.0f;
    const B::SwingReading r{(v - 3.5f) * 100.0f, heading, twist, 90};
    s.holding = false;
    s.relArm = -40;
    launch(B::makeThrow(r, m.aimDeg(), m.startX()));
    out.printf("[bowl] threw %.1f m/s heading %.1f twist %.1f from %d boards, aim %.2f\n", v, heading, twist,
               m.position(), m.aimDeg());
  } else if (strncmp(args, "aim", 3) == 0) {
    m.setAim(strtof(args + 3, nullptr));
    out.printf("[bowl] aim %.2f\n", m.aimDeg());
  } else if (strncmp(args, "pos", 3) == 0) {
    m.setPosition(atoi(args + 3));
    out.printf("[bowl] position %d boards\n", m.position());
  } else if (strncmp(args, "help", 4) == 0) {
    if (s.holding) { g->reader.cancel(); s.holding = false; }
    g->help.toggle();
    out.printf("[bowl] help %s (page %d)\n", g->help.isOpen() ? "open" : "closed", g->help.page() + 1);
  } else if (strncmp(args, "new", 3) == 0) {
    m.reset();
    s.thrown = false;
    s.view = V::View::Approach;
    out.println("[bowl] new game");
  } else {
    char marks[64] = "";
    for (int f = 0; f < 10; ++f) {
      char mk[4];
      m.card().marks(f, mk);
      strncat(marks, *mk ? mk : ".", sizeof(marks) - strlen(marks) - 2);
      strncat(marks, " ", sizeof(marks) - strlen(marks) - 1);
    }
    const B::Ball& b = m.lane().ball();
    out.printf("[bowl] mode=%d phase=%d view=%d frame=%d ball=%d score=%d card=%s rack=%03x standing=%03x pos=%d "
               "aim=%.2f ball=%.2f,%.2f gutter=%d holding=%d stage=%d draw=%uus max=%uus freeInternal=%u\n",
               (int)s.mode, (int)m.phase(), (int)s.view, m.card().frame() + 1, m.card().ball() + 1, m.card().score(),
               marks, m.rack(), m.lane().standing(), m.position(), m.aimDeg(), b.x, b.y, (int)b.gutter,
               (int)s.holding, (int)g->reader.stage(), (unsigned)(s.drawUs / (s.frames ? s.frames : 1)),
               (unsigned)s.maxDrawUs,
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  }
}
#endif

static void screenTick(uint32_t) { tick(); }

// A+B exits (system), but an A pressed by accident in the middle of a swing must not throw the game away. B's edge
// often lands a poll before A's in a deliberate A+B, so a press that has not become a swing yet lets the exit through,
// and is cancelled so B's release does not step the bowler.
static bool onChord() {
  if (g) g->help.input(H::Ev::Chord, millis(), false);   // an A hold in progress is not a panel request
  if (!s.holding || !g) return false;
  if (g->reader.keepsChord(millis())) return true;
  g->reader.cancel();
  s.holding = false;
  return false;
}

// You play by swinging, and the ball takes seconds to settle, so the system's 60 s idle return-to-home (and battery
// sleep) must not fire mid-game. Busy while there was a button, a swing or a rolling ball in the last minute.
static core::health::RuntimePolicy runtimePolicy() {
  return core::health::whileBusy(g != nullptr && (int32_t)(millis() - s.activeAt) < 60000);
}

static void buildBowlingScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  scene.addRect("bowling.bg", {0, 0, width, height}, kChrome, 0, 0);
}

static const ::scene::Screen kScreenBowling = {
    "Bowling", nullptr, nullptr, screenTick, onInput, onChord, onDraw, "game", buildBowlingScene,
};

static const ::scene::Screen* const kScreens[] = {&kScreenBowling};
App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Bowling", "bowling", &kScreenBowling, enter, resume, exitApp,
                                            "src/apps/bowling_app.cpp", kScreens, nullptr, runtimePolicy);

}  // namespace bowling
}  // namespace apps

#endif  // MENU_APP
