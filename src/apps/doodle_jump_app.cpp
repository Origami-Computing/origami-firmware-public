/**
 * @file doodle_jump_app.cpp
 * @brief Doodle Jump style IMU-controlled vertical platformer.
 */

#ifdef MENU_APP

#include "apps/doodle_jump_app.h"
#include "ui/safe_area_draw.h"
#include "ui/doodle_notebook_draw.h"
#include "ui/dial_icon.h"
#include "ui/screen_chrome.h"

#include <Arduino.h>
#include <esp_random.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "services/doodle_controls.h"
#include "games/imu_roll_control.h"
#include "hal/hal_display.h"
#include "hal/hal_imu.h"
#include "scene/screen.h"
#include "ui/scene.h"

namespace apps {
namespace doodle_jump {

namespace {

static float clampf(float value,float low,float high){return fmaxf(low,fminf(high,value));}

static constexpr uint16_t kBg = doodle_draw::paper;
static constexpr uint16_t kPanel = doodle_draw::panel;
static constexpr uint16_t kPanelText = doodle_draw::ink;

// Sample the tutorial artwork once. Trigonometric supersampling each paused
// frame delayed the input poll enough to miss a short dial press.
struct PauseDial {
  uint16_t pixels[24*24]{};
  bool ready=false;
  void fillRect(int x,int y,int w,int h,uint16_t color) {
    for(int yy=y;yy<y+h;++yy)for(int xx=x;xx<x+w;++xx)
      if(xx>=0&&xx<24&&yy>=0&&yy<24)pixels[yy*24+xx]=color;
  }
  void prepare(){if(!ready){ui::drawDialIcon(*this,12,12,.6f,kPanel);ready=true;}}
  void draw(TFT_eSprite& canvas){
    prepare();
    for(int y=0;y<24;++y)for(int x=0;x<24;){
      const auto color=pixels[y*24+x];int end=x+1;
      while(end<24&&pixels[y*24+end]==color)++end;
      canvas.drawFastHLine(128+x,160+y,end-x,color);x=end;
    }
  }
};
static PauseDial s_pauseDial;

// Landscape canvas, matching the notebook preview and physical display.
static constexpr float kScreenW = 280.0f;
static constexpr float kScreenH = 240.0f;
static constexpr float kPlayerW = 22.0f;
static constexpr float kPlayerH = 24.0f;
static constexpr float kPlatformW = 46.0f;
static constexpr float kPlatformH = 8.0f;
static constexpr uint8_t kPlatformCount = 13;
static constexpr float kGravity = -760.0f;
static constexpr float kJumpVelocity = 430.0f;
static constexpr float kVelocitySmooth = 0.35f;
// The 35px notebook character must remain below the score strip (ends at 62).
static constexpr float kCameraTargetY = 188.0f;
static constexpr float kCameraLandingY = 220.0f;
static constexpr float kCameraMaxRetreat = 96.0f;
static constexpr float kFallMargin = 34.0f;
static constexpr float kMinPlatformGap = 30.0f;
static constexpr float kMaxPlatformGap = 50.0f;
static constexpr uint32_t kMaxElapsedMs = 48u;
static constexpr int kPlayfieldTop = 63; // Includes the score strip's bottom stroke.
static constexpr uint16_t kHeader = doodle_draw::rgb(233,222,192);
static constexpr uint8_t kPelletCount = 8;
struct Pellet { float x, y; bool active; };

enum class Mode : uint8_t {
  WaitNeutral,
  Running,
  Paused,
  GameOver,
  NoImu,
};

struct Platform {
  float x = 0.0f;
  float y = 0.0f;
};

struct State {
  Mode mode = Mode::WaitNeutral;
  float playerX = 0.0f;
  float playerY = 0.0f;
  float previousPlayerY = 0.0f;
  float vx = 0.0f;
  float vy = 0.0f;
  float cameraY = 0.0f;
  float peakCameraY = 0.0f;
  float highestPlayerY = 0.0f;
  float neutralRollDeg = 0.0f;
  uint32_t score = 0;
  uint32_t highScore = 0;
  uint32_t lastMs = 0;
  bool imuOk = false;
  uint32_t landings = 0;
  uint32_t frames = 0;
  uint32_t drawUs = 0;
  Pellet pellets[kPelletCount] = {};
  uint32_t shotsFired = 0;
  uint32_t lastShotMs = 0;
  uint32_t pausedAt = 0;
  games::imu_roll::Control control;
  Platform platforms[kPlatformCount];
};

static State s;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
static bool s_testClipping = false;
#endif

static uint32_t randRange(uint32_t minValue, uint32_t maxValue) {
  if (maxValue <= minValue) return minValue;
  return minValue + (esp_random() % (maxValue - minValue + 1u));
}

static float screenYForWorld(float worldY) {
  return kScreenH - (worldY - s.cameraY);
}

static void wrapPlayerX() {
  if (s.playerX < -kPlayerW) {
    s.playerX = kScreenW;
  } else if (s.playerX > kScreenW) {
    s.playerX = -kPlayerW;
  }
}

static void sortPlatformsByHeight() {
  for (uint8_t i = 1; i < kPlatformCount; ++i) {
    Platform p = s.platforms[i];
    int j = i - 1;
    while (j >= 0 && s.platforms[j].y > p.y) {
      s.platforms[j + 1] = s.platforms[j];
      --j;
    }
    s.platforms[j + 1] = p;
  }
}

static float highestPlatformY() {
  float highest = s.platforms[0].y;
  for (uint8_t i = 1; i < kPlatformCount; ++i) {
    if (s.platforms[i].y > highest) highest = s.platforms[i].y;
  }
  return highest;
}

static void placePlatformAbove(Platform& platform, float baseY) {
  const float gap = (float)randRange((uint32_t)kMinPlatformGap,
                                    (uint32_t)kMaxPlatformGap);
  platform.y = baseY + gap;
  platform.x = (float)randRange(6, (uint32_t)(kScreenW - kPlatformW - 6.0f));
}

static void resetGameWorld() {
  s.playerX = (kScreenW - kPlayerW) * 0.5f;
  s.playerY = 72.0f;
  s.previousPlayerY = s.playerY;
  s.vx = 0.0f;
  s.vy = kJumpVelocity;
  s.cameraY = 0.0f;
  s.peakCameraY = 0.0f;
  s.highestPlayerY = s.playerY;
  s.score = 0;
  s.landings = 0;
  s.frames = 0;
  memset(s.pellets,0,sizeof(s.pellets));
  s.shotsFired = 0;

  s.platforms[0].x = (kScreenW - kPlatformW) * 0.5f;
  s.platforms[0].y = 48.0f;
  float y = s.platforms[0].y;
  for (uint8_t i = 1; i < kPlatformCount; ++i) {
    placePlatformAbove(s.platforms[i], y);
    y = s.platforms[i].y;
  }
}

static bool readControl(games::imu_roll::Control* outControl) {
  int32_t accel[3] = {};
  int32_t gyro[3] = {};
  s.imuOk = hal::imu::readAxes(accel, gyro);
  if (!s.imuOk) return false;
  if (outControl) {
    *outControl = games::imu_roll::mapAccelToControl(accel, s.neutralRollDeg,
                                                     services::doodle_controls::tuning());
  }
  return true;
}

static bool captureNeutral() {
  if (!hal::imu::isReady()) {
    s.mode = Mode::NoImu;
    s.imuOk = false;
    s.vx = 0.0f;
    return false;
  }

  int32_t accel[3] = {};
  int32_t gyro[3] = {};
  if (!hal::imu::readAxes(accel, gyro)) {
    s.mode = Mode::NoImu;
    s.imuOk = false;
    s.vx = 0.0f;
    return false;
  }

  games::imu_roll::captureNeutralDeg(accel, &s.neutralRollDeg, services::doodle_controls::tuning());
  s.control = games::imu_roll::mapAccelToControl(accel, s.neutralRollDeg,
                                                 services::doodle_controls::tuning());
  s.imuOk = true;
  s.lastMs = millis();
  Serial.printf("[doodle] neutral=%.1f deg\n", s.neutralRollDeg);
  return true;
}

static bool startGameFromCurrentPose() {
  if (!captureNeutral()) return false;
  resetGameWorld();
  s.mode = Mode::Running;
  hal::display::markAllDirty();
  return true;
}

static void recyclePlatforms() {
  // Retain platforms throughout the bounded downward camera window.
  const float bottomWorldY = fmaxf(0.0f,s.peakCameraY-kCameraMaxRetreat)-kFallMargin;
  float highest = highestPlatformY();
  for (uint8_t i = 0; i < kPlatformCount; ++i) {
    if (s.platforms[i].y < bottomWorldY) {
      placePlatformAbove(s.platforms[i], highest);
      highest = s.platforms[i].y;
    }
  }
  sortPlatformsByHeight();
}

static void handlePlatformLandings() {
  if (s.vy > 0.0f) return;

  const float playerLeft = s.playerX + 3.0f;
  const float playerRight = s.playerX + kPlayerW - 3.0f;
  for (uint8_t i = 0; i < kPlatformCount; ++i) {
    const Platform& platform = s.platforms[i];
    const float platformTop = platform.y + kPlatformH * 0.5f;
    if (s.previousPlayerY >= platformTop && s.playerY <= platformTop &&
        playerRight >= platform.x && playerLeft <= platform.x + kPlatformW) {
      s.playerY = platformTop;
      s.vy = kJumpVelocity;
      ++s.landings;
      return;
    }
  }
}

static void updateCameraAndScore() {
  if (s.playerY > s.highestPlayerY) s.highestPlayerY = s.playerY;

  const float targetCameraY = s.playerY - (kScreenH - kCameraTargetY);
  s.peakCameraY = fmaxf(s.peakCameraY,targetCameraY);
  // Keep feet in a 188..220px window. Downward follow is bounded so missing
  // platforms still carries the player offscreen and ends the game.
  s.cameraY = clampf(s.cameraY,targetCameraY,s.playerY-(kScreenH-kCameraLandingY));
  s.cameraY = clampf(s.cameraY,fmaxf(0.0f,s.peakCameraY-kCameraMaxRetreat),s.peakCameraY);

  const float climbed = s.highestPlayerY - 72.0f;
  s.score = climbed > 0.0f ? (uint32_t)(climbed * 0.25f) : 0u;
}

static void endGame() {
  if (s.score > s.highScore) s.highScore = s.score;
  s.mode = Mode::GameOver;
  s.vx = 0.0f;
  s.vy = 0.0f;
  Serial.printf("[doodle] game_over score=%lu high=%lu\n",
                (unsigned long)s.score, (unsigned long)s.highScore);
  hal::display::markAllDirty();
}

static void drawPlatform(TFT_eSprite& canvas, const Platform& platform) {
  const int y = (int)screenYForWorld(platform.y);
  if (y < 55 || y >= canvas.height()) return;
  doodle_draw::platform(canvas, (int)platform.x, y, (int)kPlatformW);
}

static void drawPlayer(TFT_eSprite& canvas) {
  // Body spans the existing 22px collision box; feet end at the landing plane.
  const uint32_t poseMs = s.mode == Mode::Paused ? s.pausedAt : millis();
  const bool firing = (s.mode == Mode::Running || s.mode == Mode::Paused) && s.shotsFired && poseMs-s.lastShotMs < 350;
  doodle_draw::sprite(canvas, firing ? doodle_art::shootingPlayer : doodle_art::player,
                     (int)s.playerX - 3, (int)screenYForWorld(s.playerY) - 34,
                     !firing && s.vx < -1.0f);
}

static void firePellet() {
  for (auto& p : s.pellets) if (!p.active) {
    p = {s.playerX + 11.0f, s.playerY + 34.0f, true};
    ++s.shotsFired;
    s.lastShotMs = millis();
    hal::display::markAllDirty();
    return;
  }
}

static void drawPellets(TFT_eSprite& canvas) {
  if (s.mode != Mode::Running && s.mode != Mode::Paused) return;
  for (const auto& p : s.pellets) if (p.active) {
    canvas.fillEllipse((int)p.x,(int)screenYForWorld(p.y),3,3,doodle_draw::rgb(213,218,69));
    canvas.drawEllipse((int)p.x,(int)screenYForWorld(p.y),3,3,doodle_draw::ink);
  }
}

static void drawWorld(TFT_eSprite& canvas) {
  canvas.setViewport(0,kPlayfieldTop,canvas.width(),canvas.height()-kPlayfieldTop,false);
  if (s.mode != Mode::WaitNeutral) {
    for (uint8_t i = 0; i < kPlatformCount; ++i) drawPlatform(canvas, s.platforms[i]);
    drawPlayer(canvas);
    drawPellets(canvas);
  }
  canvas.setViewport(0,38,canvas.width(),canvas.height()-38,false);
}

#ifdef ORIGAMI_DEV_SERIAL_INPUT
static void checkWorldClipping(TFT_eSprite& canvas) {
  // Exercise actual world drawing before any header overpaint can hide a leak.
  const State saved = s;
  canvas.resetViewport();
  // System chrome is cached: preserve it rather than assuming NavHandler will
  // repaint every pixel after this diagnostic. Hash it to detect world leaks.
  uint32_t chromeBefore=2166136261u;
  for(int y=0;y<38;++y)for(int x=0;x<canvas.width();++x)
    chromeBefore=(chromeBefore^canvas.readPixel(x,y))*16777619u;
  canvas.fillRect(0,38,canvas.width(),canvas.height()-38,0xF81F);
  s.mode=Mode::Running;s.cameraY=0;s.playerY=190;s.playerX=70;
  s.shotsFired=1;s.lastShotMs=millis();
  for (auto& p : s.pellets) p.active=false;
  const int ys[]={30,50,62,63,70};
  for (unsigned i=0;i<5;++i) s.pellets[i]={140.0f,(float)(240-ys[i]),true};
  s.platforms[0].x=180.0f;s.platforms[0].y=180.0f;
  drawWorld(canvas);
  canvas.resetViewport();
  unsigned leaks=0,visible=0;
  uint32_t chromeAfter=2166136261u;
  for(int y=0;y<38;++y)for(int x=0;x<canvas.width();++x)
    chromeAfter=(chromeAfter^canvas.readPixel(x,y))*16777619u;
  if(chromeAfter!=chromeBefore)++leaks;
  for(int y=38;y<kPlayfieldTop;++y)for(int x=0;x<canvas.width();++x)
    if(canvas.readPixel(x,y)!=0xF81F)++leaks;
  for(int y=kPlayfieldTop;y<75;++y)for(int x=135;x<145;++x)
    if(canvas.readPixel(x,y)!=0xF81F)++visible;
  Serial.printf("[doodle] clipping leaks=%u visible=%u\n",leaks,visible);
  s=saved;
  canvas.setViewport(0,38,canvas.width(),canvas.height()-38,false);
}
#endif

static void drawHud(TFT_eSprite& canvas) {
  canvas.fillRect(0, 38, canvas.width(), kPlayfieldTop-38, kHeader);
  canvas.drawLine(0,61,82,62,doodle_draw::rgb(119,112,82));
  canvas.drawLine(82,62,190,61,doodle_draw::rgb(119,112,82));
  canvas.drawLine(190,61,279,62,doodle_draw::rgb(119,112,82));
  char line[40];
  snprintf(line,sizeof(line),"%lu",(unsigned long)s.score);
  doodle_draw::text(canvas,line,14,56,doodle_art::font16);
  snprintf(line,sizeof(line),"best %lu",(unsigned long)s.highScore);
  doodle_draw::text(canvas,line,265,55,doodle_art::font11,doodle_draw::ink,2);
}

static void drawNoImu(TFT_eSprite& canvas) {
  canvas.fillRoundRect(30,87,220,100,4,kPanel);
  canvas.drawRoundRect(30,87,220,100,4,kPanelText);
  doodle_draw::text(canvas,"motion sensor unavailable",140,114,doodle_art::font13,kPanelText,1);
  doodle_draw::text(canvas,"press dial to retry",140,143,doodle_art::font13,kPanelText,1);
  doodle_draw::text(canvas,"both buttons: back",140,169,doodle_art::font13,kPanelText,1);
}

}  // namespace

static void prepareWorkflow() {
  Serial.println("[doodle] enter");
  const auto loaded = services::doodle_controls::load();
  if (loaded == services::doodle_controls::LoadResult::Loaded) {
    hal::display::markAllDirty();
    Serial.println("[doodle] loaded_saved_tuning=1");
  } else if (loaded == services::doodle_controls::LoadResult::Invalid) {
    Serial.println("[doodle] loaded_saved_tuning=invalid_using_defaults");
  }
  const uint32_t high = s.highScore;
  memset(&s, 0, sizeof(s));
  s.highScore = high;
  s.lastMs = millis();
  resetGameWorld();
  s.mode = hal::imu::isReady() ? Mode::WaitNeutral : Mode::NoImu;
  s.imuOk = hal::imu::isReady();
  hal::display::markAllDirty();
}

static void exitApp() {
  Serial.println("[doodle] exit");
}

void tick() {
  const uint32_t now = millis();
  uint32_t elapsed = now - s.lastMs;
  if (elapsed > kMaxElapsedMs) elapsed = kMaxElapsedMs;
  s.lastMs = now;

  if (s.mode != Mode::Running) return;

  games::imu_roll::Control control;
  if (!readControl(&control)) {
    s.mode = Mode::NoImu;
    s.vx = 0.0f;
    s.vy = 0.0f;
    hal::display::markAllDirty();
    return;
  }

  ++s.frames;
  const float dt = (float)elapsed / 1000.0f;
  s.control = control;
  s.previousPlayerY = s.playerY;
  s.vx += (s.control.velocityPxPerSec - s.vx) * kVelocitySmooth;
  s.playerX += s.vx * dt;
  wrapPlayerX();

  s.vy += kGravity * dt;
  s.playerY += s.vy * dt;
  handlePlatformLandings();
  updateCameraAndScore();
  for (auto& p : s.pellets) if (p.active) {
    p.y += 520.0f * dt;
    if (screenYForWorld(p.y)+3 < kPlayfieldTop) p.active = false;
  }
  recyclePlatforms();

  if (screenYForWorld(s.playerY) > kScreenH + kFallMargin) {
    endGame();
    return;
  }

  hal::display::markAllDirty();
}

void onInput(hal::input::InputEvent ev) {
  using ET = hal::input::EventType;

  if (ev.type == ET::ButtonC_Tap) {
    if (s.mode == Mode::Running) {
      s.mode=Mode::Paused;s.pausedAt=millis();
    } else if (s.mode == Mode::Paused) {
      const uint32_t now=millis();
      s.lastShotMs+=now-s.pausedAt;s.lastMs=now;s.mode=Mode::Running;
    } else startGameFromCurrentPose();
    hal::display::markAllDirty();
    return;
  }
  if (ev.type == ET::ButtonB_Tap) {
    if (s.mode == Mode::Running) firePellet();
    return;
  }

  if (ev.type == ET::ButtonA_Tap) {
    if (s.mode == Mode::GameOver || s.mode == Mode::WaitNeutral) {
      startGameFromCurrentPose();
    } else if (s.mode == Mode::Running) {
      captureNeutral();
      hal::display::markAllDirty();
    }
    return;
  }
}

void onDraw(TFT_eSprite& canvas) {
  const uint32_t started = micros();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  if(s_testClipping){s_testClipping=false;checkWorldClipping(canvas);}
#endif
  doodle_draw::background(canvas);
  setSceneChromeBackground(kHeader);
  // World objects cannot paint either header, regardless of drawing order.
  drawWorld(canvas);
  // Restore the app viewport; NavHandler paints system chrome after onDraw.
  drawHud(canvas);
  if (s.mode == Mode::WaitNeutral) doodle_draw::ready(canvas);
  else if (s.mode == Mode::Paused) {
    canvas.fillRoundRect(30,90,220,98,5,kPanel);
    canvas.drawRoundRect(30,90,220,98,5,kPanelText);
    doodle_draw::text(canvas,"paused",140,119,doodle_art::font25,kPanelText,1);
    doodle_draw::text(canvas,"press dial to resume",140,148,doodle_art::font13,kPanelText,1);
    s_pauseDial.draw(canvas);
  }
  else if (s.mode == Mode::GameOver) doodle_draw::gameOver(canvas,s.score,s.highScore);
  else if (s.mode == Mode::NoImu) drawNoImu(canvas);
  s.drawUs = micros() - started;
  hal::display::markAllDirty();
}

#ifdef ORIGAMI_DEV_SERIAL_INPUT
void testClipping() { s_testClipping=true;hal::display::markAllDirty(); }
void printStatus(Print& out) {
  const char* mode = s.mode == Mode::WaitNeutral ? "ready" :
      s.mode == Mode::Running ? "running" : s.mode == Mode::Paused ? "paused" : s.mode == Mode::GameOver ? "game-over" : "no-imu";
  unsigned active = 0;
  for (const auto& p : s.pellets) if (p.active) ++active;
  out.printf("[doodle] status mode=%s imu=%d score=%lu best=%lu x=%.2f y=%.2f vx=%.2f vy=%.2f landings=%lu frames=%lu draw_us=%lu shots=%lu pellets=%u screen_y=%.2f camera=%.2f camera_peak=%.2f\n",
      mode,s.imuOk,(unsigned long)s.score,(unsigned long)s.highScore,
      s.playerX,s.playerY,s.vx,s.vy,(unsigned long)s.landings,
      (unsigned long)s.frames,(unsigned long)s.drawUs,(unsigned long)s.shotsFired,active,
      screenYForWorld(s.playerY),s.cameraY,s.peakCameraY);
}

// Developer-only fault injection exercises the normal tick -> endGame path.
// Does not write tuning or calibration preferences or bypass the IMU read.
void testFall() {
  if (s.mode != Mode::Running) return;
  s.playerY = fmaxf(0.0f,s.peakCameraY-kCameraMaxRetreat) - kFallMargin - 4.0f;
  s.vy = -kJumpVelocity;
}
#endif

static void screenTick(uint32_t) {
  tick();
}

static void buildDoodleScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  scene.addRect("doodle.bg", {0, 0, width, height}, kBg, 0, 0);
  scene.addLabel("doodle.title", {0, 0, width, 20}, "Doodle Jump",
                 kPanelText, kPanel, 2, ui::scene::TextAlign::TopLeft, 1);
}

static const ::scene::Screen kScreenDoodle = {
    "Doodle Jump",
    nullptr,
    nullptr,
    screenTick,
    onInput,
    nullptr,
    onDraw,
    "game",
    buildDoodleScene,
};

static const ::scene::Screen* const kScreens[] = {&kScreenDoodle};
App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Doodle Jump", "doodle-jump", &kScreenDoodle, prepareWorkflow, prepareWorkflow, exitApp,
                            "src/apps/doodle_jump_app.cpp", kScreens, nullptr, nullptr);

}  // namespace doodle_jump
}  // namespace apps

#endif  // MENU_APP
