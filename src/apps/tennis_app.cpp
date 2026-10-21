/**
 * @file tennis_app.cpp
 * @brief Tennis spike (throwaway): swing the kami like a racket against a capped-speed AI.
 *
 * Enter -> hold still (calibrates gyro bias and "up") -> C (dial press) tosses; swing at the top of the toss for a
 * power serve. In a rally you run automatically; swing left or right (forehand/backhand) as the ball arrives.
 * B recalibrates. Logic is in core/tennis (host-tested); this file only reads the IMU, draws and buzzes.
 */

#if defined(MENU_APP) && defined(KAMI_GAMES) && KAMI_GAMES  // N16R8 only (platformio.ini)

#include "apps/tennis_app.h"
#include "core/tennis.h"
#include "games/hold_help.h"
#include "games/tennis_help.h"
#include "games/tennis_view.h"
#include "core/health_policy.h"
#include "ui/safe_area_draw.h"
#include "ui/screen_chrome.h"
#include "ui/doodle_notebook_draw.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "hal/hal_display.h"
#include "services/vibration.h"
#include "scene/screen.h"
#include "services/motion.h"
#include "ui/scene.h"

namespace apps {
namespace tennis {

namespace T = core::tennis;
namespace V = games::tennis_view;
namespace H = games::tennis_help;


enum class Mode : uint8_t { NoImu, Calibrating, Playing };

struct State {
  Mode mode;
  uint32_t calStart;
  uint32_t msgUntil;
  char msg[28];
  uint32_t ticks, samples, statsAt, drawUs, tickUs, maxGapMs, lastTickMs;
  // onDraw breakdown over the 5 s stats window: time per part, pixels repainted, display rows flushed
  uint32_t draws, measureUsSum, bgUsSum, spriteUsSum, hudUsSum, drawUsSum, maxDrawUs, areaSum, rowsSum, rectsSum;
  float yaw;
  uint32_t swingAt, aiSwingAt;       // racket animations
  uint32_t activeAt;                 // last swing / button: swings are the user's activity here
  char plan[33];                     // dev autoplay: queued landing sides, 'L' 'R' 'M'
  uint8_t planPos;
  bool autoPlay;
  uint32_t autoContact;              // the contact we already swung at
  T::Side swingSide, aiSwingSide;
  T::Event lastShown;
  games::HoldToOpen holdA;           // hold A: the how-to-play panel
  H::Panel help;                     // while it is open the game is paused
};

static State s;
static T::SwingDetector s_det;
static T::Game s_game;
static services::motion::GyroFifo s_fifo;
static V::Retained* s_view = nullptr;         // what the last frame drew (PSRAM, while the app is open)

static void say(const char* text, uint32_t ms = 1100) {
  snprintf(s.msg, sizeof(s.msg), "%s", text);
  s.msgUntil = millis() + ms;
}

static void buzz(uint32_t ms) {
  services::vibration::pulse(services::vibration::Owner::Tennis, ms);
}

static void beginCalibration() {
  s.mode = Mode::Calibrating;
  s.calStart = millis();
  s_det.calibrateBegin();
}

static H::Mode helpMode() {
  return s.mode == Mode::NoImu ? H::Mode::NoImu : (s.mode == Mode::Calibrating ? H::Mode::Calibrating : H::Mode::Playing);
}

// The how-to-play panel opens only while the game waits for the player (never over a rally); the game pauses.
static bool openHelp(uint32_t now) {
  if (s.help.isOpen() || !H::canOpen(helpMode(), s_game.state())) return false;
  s.help.open(now);
  s.activeAt = now;
  Serial.printf("[tennis] help open (phase %u)\n", (unsigned)s_game.state().phase);
  return true;
}

static void closeHelp(uint32_t now) {
  if (!s.help.isOpen()) return;
  s_game.shiftTime(s.help.close(now));             // the game carries on where it was
  s.activeAt = now;
  if (s.mode == Mode::Calibrating) beginCalibration();   // the kami moved while reading: calibrate again
  Serial.println("[tennis] help closed");
}

static void describe(T::Event ev) {
  char buf[28];
  const int16_t e = s_game.state().lastTimingMs;
  switch (ev) {
    case T::Event::PowerServe: say("POWER SERVE!"); buzz(90); break;
    case T::Event::Serve: say(e < 0 ? "Serve (early)" : "Serve (late)"); buzz(40); break;
    case T::Event::Fault: say(e < 0 ? "Fault: too early" : "Fault: too late"); break;
    case T::Event::DoubleFault: say("Double fault"); break;
    case T::Event::Hit: snprintf(buf, sizeof(buf), "%s %+d ms", e < -60 ? "Early" : (e > 60 ? "Late" : "Nice!"), e); say(buf, 700); buzz(45); break;
    case T::Event::Whiff: snprintf(buf, sizeof(buf), "Miss %s %d ms", e < 0 ? "early" : "late", e < 0 ? -e : e); say(buf); break;
    case T::Event::WrongSide: say(s_game.sideNeeded() == T::Side::Forehand ? "Forehand side!" : "Backhand side!"); break;
    case T::Event::TooFar: say("Couldn't reach it"); break;
    case T::Event::AiMiss: say("Winner!"); break;
    case T::Event::Out: say(s_game.state().lastHitByPlayer ? "Out" : "AI hit it out"); break;
    case T::Event::Net: say(s_game.state().lastHitByPlayer ? "Into the net" : "AI netted it"); break;
    case T::Event::PlayerPoint: say("Your point"); break;
    case T::Event::AiPoint: say("AI's point"); break;
    case T::Event::PlayerWinsMatch: say("you win! dial: new match", 60000); buzz(200); break;
    case T::Event::AiWinsMatch: say("cpu wins. dial: new match", 60000); break;
    default: break;
  }
}

static void prepareWorkflow() {
  memset(&s, 0, sizeof(s));
  s.activeAt = millis();
  s_game.reset(millis(), esp_random());
  if (!s_view) s_view = (V::Retained*)heap_caps_calloc(1, sizeof(V::Retained), MALLOC_CAP_SPIRAM);
  if (s_view) V::invalidate(*s_view);
  if (!s_view || !s_fifo.open()) {
    s_fifo.close();
    s.mode = Mode::NoImu;
  } else {
    beginCalibration();
  }
  Serial.printf("[tennis] enter state=%u+%u+%u bytes\n", (unsigned)sizeof(s), (unsigned)sizeof(s_det),
                (unsigned)sizeof(s_game));
  hal::display::markAllDirty();
}

// Back from an overlay or alert: keep the match, repaint everything, re-take the calibration (the kami was probably
// moved). It used to start a new match.
static void resume() {
  if (!s_fifo.active() || !s_view || s.mode == Mode::NoImu) { prepareWorkflow(); return; }
  s.activeAt = millis();
  V::invalidate(*s_view);                           // an overlay may have drawn over the canvas
  if (!s_fifo.reset()) s.mode = Mode::NoImu;
  else beginCalibration();
  hal::display::markAllDirty();
}

static void exitApp() {
  s_fifo.close();
  heap_caps_free(s_view);
  s_view = nullptr;
  services::vibration::release(services::vibration::Owner::Tennis);
  Serial.println("[tennis] exit");
}

static void describe(T::Event ev);

// Dev autoplay: swing for the player with the timing that lands the ball where the plan (or the bot) wants.
static void autoSwing(uint32_t now) {
  const bool queued = s.planPos < strlen(s.plan);
  if (!queued && !s.autoPlay) return;
  const T::State& st = s_game.state();
  const T::Tuning& tu = s_game.tuning();
  if (st.phase == T::Phase::ServeReady && st.playerServes && (int32_t)(now - st.phaseMs) > 900) {
    if (s_game.startToss(now) == T::Event::TossStarted) say("serving...", 600);
    return;
  }
  if (st.phase == T::Phase::Toss) {
    const uint32_t apex = st.phaseMs + (uint32_t)(tu.tossApexS * 1000.0f);
    if ((int32_t)(now - apex) >= 20) {
      s.swingAt = now; s.swingSide = T::Side::Forehand; s.activeAt = now;
      const T::Event ev = s_game.swing(T::Swing{T::Side::None, 700, 800, apex + 10}, now);
      if (ev != T::Event::None) describe(ev);
    }
    return;
  }
  const uint32_t contact = s_game.playerContactMs(now);
  if (!contact || contact == s.autoContact) return;
  float target;                                            // landing x
  char want = queued ? s.plan[s.planPos] : (st.ai.x > 0 ? 'L' : 'R');
  target = want == 'L' ? -0.75f : (want == 'R' ? 0.75f : 0.0f);
  const T::Side side = s_game.sideNeeded();
  const float pull = side == T::Side::Forehand ? -1.0f : 1.0f;
  const int32_t offset = (int32_t)(tu.timingCenterMs + (-target / (pull * tu.aimSpread)) * tu.aimSpanMs);
  if ((int32_t)(now - (contact + offset)) < 0) return;      // not yet: swing at the planned moment
  s.autoContact = contact;
  if (queued) ++s.planPos;
  s.swingAt = now; s.swingSide = side; s.activeAt = now;
  const T::Event ev = s_game.swing(T::Swing{side, 700, 800, contact + offset}, now);
  Serial.printf("[tennis] auto %c %s offset=%dms -> event=%d aiX=%.2f\n", want,
                side == T::Side::Forehand ? "forehand" : "backhand", (int)offset, (int)ev, st.ai.x);
  if (ev != T::Event::None) describe(ev);
}

void tick() {
  const uint32_t started = micros();
  const uint32_t now = millis();
  if (s.lastTickMs && now - s.lastTickMs > s.maxGapMs) s.maxGapMs = now - s.lastTickMs;
  s.lastTickMs = now;
  if (s.holdA.tick(now)) openHelp(now);
  if (s.mode == Mode::NoImu) return;

  // Every gyro sample since the last frame (104 Hz FIFO), oldest first; the newest is ~now.
  const int n = s_fifo.poll();
  const auto fifo = s_fifo.samples();
  if (n < 0) return;
  s.samples += n;

  if (s.help.isOpen()) {
    // Paused: the samples are drained and dropped (no swings, no calibration), and the game isn't stepped.
  } else if (s.mode == Mode::Calibrating) {
    int32_t a[3] = {};
    if (!s_fifo.readAcceleration(a)) return;
    const float acc[3] = {(float)a[0], (float)a[1], (float)a[2]};
    for (int i = 0; i < n; ++i) {
      const float gyro[3] = {fifo[i][0] / 1000.0f, fifo[i][1] / 1000.0f, fifo[i][2] / 1000.0f};
      s_det.calibrateSample(acc, gyro);
    }
    if (now - s.calStart >= 1000) {
      if (s_det.calibrateEnd()) {
        s.mode = Mode::Playing;
        const float* u = s_det.up();
        Serial.printf("[tennis] calibrated up=%.2f,%.2f,%.2f\n", u[0], u[1], u[2]);
        say("ready! press dial to serve", 1800);
      } else {
        say("Hold still...", 800);
        beginCalibration();
      }
    }
  } else {
    for (int i = 0; i < n; ++i) {
      const uint32_t at = now - (uint32_t)((n - 1 - i) * 1000 / 104);
      const float gyro[3] = {fifo[i][0] / 1000.0f, fifo[i][1] / 1000.0f, fifo[i][2] / 1000.0f};
      T::Swing sw;
      if (!s_det.update(at, gyro, &sw)) continue;
      Serial.printf("[tennis] swing side=%d yaw=%.0f total=%.0f peak=%u now=%u phase=%u contact=%u\n", (int)sw.side,
                    sw.peakDps, sw.totalDps, (unsigned)sw.peakMs, (unsigned)now, (unsigned)s_game.state().phase,
                    (unsigned)s_game.playerContactMs(now));
      s.swingAt = now;
      s.swingSide = sw.side;
      s.activeAt = now;
      const T::Side needed = s_game.sideNeeded();
      const T::Event ev = s_game.swing(sw, now);
      Serial.printf("[tennis]   -> event=%d needed=%d timing=%d ballX=%.2f playerX=%.2f\n", (int)ev, (int)needed,
                    (int)s_game.state().lastTimingMs, s_game.state().ball.x, s_game.state().player.x);
      if (ev != T::Event::None) describe(ev);
    }
    s.yaw = s_det.lastYawDps();
    autoSwing(now);
    const T::Event ev = s_game.step(now);
    if (ev == T::Event::AiHit) { s.aiSwingAt = now; s.aiSwingSide = s_game.state().ball.x > s_game.state().ai.x ? T::Side::Forehand : T::Side::Backhand; }
    if (ev != T::Event::None) {
      describe(ev);
      Serial.printf("[tennis] event=%d games %u-%u points %u-%u\n", (int)ev, s_game.state().playerGames,
                    s_game.state().aiGames, s_game.state().playerPoints, s_game.state().aiPoints);
    }
  }

  ++s.ticks;
  s.tickUs = micros() - started;
  if (now - s.statsAt >= 5000) {
    const uint32_t d = s.draws ? s.draws : 1;
    Serial.printf("[tennis] fps=%.1f gyroHz=%.0f maxGap=%ums tick=%uus draw avg=%uus (measure %u bg %u sprites %u "
                  "hud %u; repaint %u px in %u.%u rects, %u rows) max=%uus freeInternal=%u largest=%u\n",
                  s.ticks * 1000.0f / (now - s.statsAt + 1), s.samples * 1000.0f / (now - s.statsAt + 1),
                  (unsigned)s.maxGapMs, (unsigned)s.tickUs, (unsigned)(s.drawUsSum / d), (unsigned)(s.measureUsSum / d),
                  (unsigned)(s.bgUsSum / d), (unsigned)(s.spriteUsSum / d), (unsigned)(s.hudUsSum / d),
                  (unsigned)(s.areaSum / d), (unsigned)(s.rectsSum / d), (unsigned)(s.rectsSum * 10 / d % 10),
                  (unsigned)(s.rowsSum / d), (unsigned)s.maxDrawUs,
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    s.draws = s.measureUsSum = s.bgUsSum = s.spriteUsSum = s.hudUsSum = s.drawUsSum = s.maxDrawUs = 0;
    s.areaSum = s.rowsSum = s.rectsSum = 0;
    s.ticks = 0;
    s.samples = 0;
    s.maxGapMs = 0;
    s.statsAt = now;
  }
}

// ---- drawing: games/tennis_view repaints only what changed, and only those rows go to the display ----

void onDraw(TFT_eSprite& canvas) {
  const uint32_t started = micros();
  setSceneChromeBackground(doodle_draw::rgb(95, 176, 234));
  if (!s_view) {                                          // out of PSRAM: plain sky
    canvas.fillRect(0, 38, canvas.width(), canvas.height() - 38, doodle_draw::rgb(95, 176, 234));
    return;
  }
  const T::State& st = s_game.state();
  const bool serving = st.phase == T::Phase::ServeReady && st.playerServes;
  const char* msg = (int32_t)(millis() - s.msgUntil) < 0 ? s.msg : (serving ? "press dial: toss" : "");
  if (s.mode == Mode::Calibrating) msg = "hold still...";
  if (s.mode == Mode::NoImu) msg = "no motion sensor";
  const bool helpOpen = s.help.isOpen();
  V::Scene sc;
  memset(&sc, 0, sizeof(sc));
  sc.nowMs = millis();
  sc.swingAt = s.swingAt;
  sc.aiSwingAt = s.aiSwingAt;
  sc.swingSide = s.swingSide;
  sc.aiSwingSide = s.aiSwingSide;
  sc.message = helpOpen ? "" : msg;
  sc.hint = !helpOpen && H::canOpen(helpMode(), st) ? H::kHint : nullptr;
  sc.helpOpen = helpOpen;
  sc.helpPage = (uint8_t)s.help.page();
  V::Timing tm;
  memset(&tm, 0, sizeof(tm));
  tm.clockUs = []() -> uint32_t { return (uint32_t)micros(); };
  V::draw((uint16_t*)canvas.getPointer(), s_game, sc, s_view, &tm);
  const uint32_t us = micros() - started;
  s.drawUs = us;
  if (us > s.maxDrawUs) s.maxDrawUs = us;
  s.drawUsSum += us;
  s.measureUsSum += tm.measureUs;
  s.bgUsSum += tm.bgUs;
  s.spriteUsSum += tm.spritesUs;
  s.hudUsSum += tm.hudUs;
  s.areaSum += tm.area;
  s.rowsSum += tm.rows;
  s.rectsSum += tm.rects;
  ++s.draws;
  // Only the rows that changed go to the display (none when nothing moved).
  for (int k = 0; k < s_view->rects; ++k) hal::display::markDirty(s_view->rect[k].y0, s_view->rect[k].y1 - s_view->rect[k].y0);
}

void onInput(hal::input::InputEvent ev) {
  using ET = hal::input::EventType;
  const uint32_t now = millis();
  s.activeAt = now;
  // Hold A: how to play (games/hold_help.h: after a long hold, the release and the tap that follows are ignored).
  switch (ev.type) {
    case ET::ButtonA_Press: s.holdA.press(now); return;
    case ET::ButtonA_Release:
      if (s.holdA.release(now) && s.holdA.opensOnRelease()) openHelp(now);
      return;
    case ET::ButtonA_Tap:
      if (!s.holdA.swallowTap()) closeHelp(now);     // A has no other use in tennis
      return;
    case ET::Chord_AB: s.holdA.cancel(); return;
    default: break;
  }
  if (s.help.isOpen()) {                               // the panel takes every input: dial pages, B or press closes
    if (ev.type == ET::Scroll) s.help.turn(ev.delta);
    else if (ev.type == ET::ButtonB_Tap || ev.type == ET::ButtonC_Tap) closeHelp(now);
    return;
  }
  if (ev.type == ET::ButtonC_Tap) {
    if (s.mode != Mode::Playing) return;
    const T::Phase p = s_game.state().phase;
    if (p == T::Phase::MatchOver) { s_game.reset(millis(), esp_random()); say("New match", 900); return; }
    if (s_game.startToss(millis()) == T::Event::TossStarted) say("swing at the top!", 700);
    return;
  }
  if (ev.type == ET::ButtonB_Tap && s.mode != Mode::NoImu) {
    beginCalibration();
    return;
  }
}

#ifdef ORIGAMI_DEV_SERIAL_INPUT
void devCommand(const char* args, Print& out) {
  while (*args == ' ') ++args;
  if (strncmp(args, "play", 4) == 0) {
    size_t n = 0;
    for (const char* p = args + 4; *p && n < sizeof(s.plan) - 1; ++p) {
      const char c = (char)toupper((unsigned char)*p);
      if (c == 'L' || c == 'R' || c == 'M') s.plan[n++] = c;
    }
    s.plan[n] = 0;
    s.planPos = 0;
    out.printf("[tennis] plan %s (%u shots)\n", s.plan, (unsigned)n);
  } else if (strncmp(args, "help", 4) == 0) {
    const uint32_t now = millis();
    if (s.help.isOpen()) closeHelp(now);
    else if (!openHelp(now)) out.printf("[tennis] help: only while the game waits for you (phase %u)\n",
                                        (unsigned)s_game.state().phase);
    else if (args[4] == ' ' && args[5] >= '1' && args[5] <= '9')
      for (int i = 1; i < args[5] - '0'; ++i) s.help.turn(1);
    out.printf("[tennis] help=%d page=%d\n", s.help.isOpen() ? 1 : 0, s.help.page() + 1);
  } else if (strncmp(args, "auto", 4) == 0) {
    s.autoPlay = strstr(args, "off") == nullptr;
    out.printf("[tennis] auto=%d\n", s.autoPlay ? 1 : 0);
  } else {
    const T::State& st = s_game.state();
    char you[8], ai[8];
    T::Game::scoreText(st.playerPoints, st.aiPoints, you, sizeof(you));
    T::Game::scoreText(st.aiPoints, st.playerPoints, ai, sizeof(ai));
    out.printf("[tennis] mode=%u phase=%u games %u-%u points %s-%s ball=%.2f,%.2f,%.2f you=%.2f ai=%.2f "
               "last=%d plan=%s@%u auto=%d\n", (unsigned)s.mode, (unsigned)st.phase, st.playerGames, st.aiGames, you,
               ai, st.ball.x, st.ball.z, st.ball.h, st.player.x, st.ai.x, (int)st.lastEvent, s.plan,
               (unsigned)s.planPos, s.autoPlay ? 1 : 0);
  }
}
#endif

static void screenTick(uint32_t) { tick(); }

// You play by swinging, not pressing buttons, so the system's 60 s idle return-to-home (and battery sleep) would
// fire mid-rally. Stay busy while there was a swing or button in the last minute; set down, it idles as usual.
static core::health::RuntimePolicy runtimePolicy() {
  return core::health::whileBusy(s.help.isOpen() ||
                                 (s.mode != Mode::NoImu && (int32_t)(millis() - s.activeAt) < 60000));
}

static void buildTennisScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  scene.addRect("tennis.bg", {0, 0, width, height}, doodle_draw::rgb(95, 176, 234), 0, 0);
}

static const ::scene::Screen kScreenTennis = {
    "Tennis", nullptr, nullptr, screenTick, onInput, nullptr, onDraw, "game", buildTennisScene,
};

static const ::scene::Screen* const kScreens[] = {&kScreenTennis};
App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Tennis", "tennis", &kScreenTennis, prepareWorkflow, resume, exitApp,
                                            "src/apps/tennis_app.cpp", kScreens, nullptr, runtimePolicy);

}  // namespace tennis
}  // namespace apps

#endif  // MENU_APP
