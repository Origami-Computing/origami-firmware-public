/**
 * @file dumb_flappy_bird_app.cpp
 * @brief Dumb Flappy Bird: steer the bird vertically by yelling at the mic.
 */

#ifdef MENU_APP

#include "apps/dumb_flappy_bird_app.h"
#include "ui/safe_area_draw.h"
#include "ui/screen_chrome.h"

#include <Arduino.h>
#include <esp_random.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "services/microphone_stream.h"
#include "hal/hal_display.h"
#include "scene/screen.h"
#include "ui/scene.h"

namespace apps {
namespace dumb_flappy_bird {

namespace {

static services::audio::MicrophoneStream s_microphone;

static constexpr uint16_t kSky = 0x7DDF;
static constexpr uint16_t kSkyDark = 0x5D1C;
static constexpr uint16_t kCloud = 0xFFFF;
static constexpr uint16_t kGround = 0xC5A6;
static constexpr uint16_t kGroundDark = 0x7BE0;
static constexpr uint16_t kPipe = 0x05E0;
static constexpr uint16_t kPipeDark = 0x03A0;
static constexpr uint16_t kPipeLight = 0x87F0;
static constexpr uint16_t kBird = 0xFFE0;
static constexpr uint16_t kBirdWing = 0xFD20;
static constexpr uint16_t kBeak = 0xFBE0;
static constexpr uint16_t kInk = 0x0000;
static constexpr uint16_t kPanel = 0x0000;
static constexpr uint16_t kPanelText = 0xFFFF;
static constexpr uint16_t kWarn = 0xFBE0;

static constexpr float kDbfsToApproxSpl = 104.0f;
static constexpr float kControlLowDb = 35.0f;
static constexpr float kControlHighDb = 80.0f;
static constexpr float kBirdX = 62.0f;
static constexpr float kBirdRadius = 9.0f;
static constexpr float kPipeW = 34.0f;
static constexpr float kPipeGap = 78.0f;
static constexpr float kPipeSpacing = 128.0f;
static constexpr float kPipeSpeed = 72.0f;
static constexpr float kGroundH = 28.0f;
static constexpr float kTopPad = 63.0f;
static constexpr int kWidth = 280, kHeight = 240;
static constexpr uint8_t kPipeCount = 4;
static constexpr uint32_t kMaxElapsedMs = 48u;

enum class Mode : uint8_t {
  Ready,
  Calibrating,
  Running,
  Paused,
  GameOver,
  NoMic,
};

struct Pipe {
  float x = 0.0f;
  float gapY = 0.0f;
  bool scored = false;
};

struct State {
  Mode mode = Mode::Running;
  bool active = false;
  bool micOk = false;
  uint32_t lastMs = 0;
  uint32_t lastMicMs = 0;
  uint32_t samplesInWindow = 0;
  uint64_t squareSum = 0;
  uint32_t absPeak = 0;
  float db = 18.0f;
  float displayDb = 35.0f;
  float peakDb = 18.0f;
  float birdY = 140.0f;
  float previousBirdY = 140.0f;
  float targetY = 140.0f;
  float control = 0.0f;
  float baselineDb = 35.0f;
  bool calibrated = false;
  uint32_t calibrationStarted = 0;
  uint8_t calibrationCount = 0;
  float calibrationWindows[64] = {};
  Mode afterCalibration = Mode::Ready;
  float groundOffset = 0.0f;
  uint16_t score = 0;
  uint16_t highScore = 0;
  Pipe pipes[kPipeCount];
  char dbText[20] = {};
};

static State s;

static float clampf(float value, float lo, float hi) {
  if (value < lo) return lo;
  if (value > hi) return hi;
  return value;
}

static uint32_t randRange(uint32_t minValue, uint32_t maxValue) {
  if (maxValue <= minValue) return minValue;
  return minValue + (esp_random() % (maxValue - minValue + 1u));
}

static float dbFromRms(float rms) {
  if (rms < 1.0f) rms = 1.0f;
  const float dbfs = 20.0f * log10f(rms / 32768.0f);
  return clampf(dbfs + kDbfsToApproxSpl, 18.0f, 112.0f);
}

static float voiceControl(float db, float baseline) {
  // A 3dB margin rejects room fluctuations; 24dB above that spans flight.
  return clampf((db-baseline-3.0f)/24.0f,0.0f,1.0f);
}

static float roomBaseline(float* values, uint8_t count) {
  for(uint8_t i=1;i<count;++i) {
    const float v=values[i];int j=i-1;
    while(j>=0&&values[j]>v){values[j+1]=values[j];--j;}
    values[j+1]=v;
  }
  return values[(count-1)*3/4]; // Ignore short loud transients.
}

static void beginCalibration() {
  if(!s.micOk)return;
  s.afterCalibration=(s.mode==Mode::Running||s.mode==Mode::Paused)?Mode::Paused:Mode::Ready;
  s.mode=Mode::Calibrating;s.calibrationStarted=millis();s.calibrationCount=0;
  s.samplesInWindow=0;s.squareSum=0;s.absPeak=0;
  hal::display::markAllDirty();
}

static float yForDb(float db, int16_t height) {
  const float topY = kTopPad + kBirdRadius + 6.0f;
  const float bottomY = (float)height - kGroundH - kBirdRadius - 4.0f;
  const float t = voiceControl(db,s.baselineDb);
  return bottomY - t * (bottomY - topY);
}

static float randomGapY(int16_t height) {
  const float top = kTopPad + kPipeGap * 0.5f + 12.0f;
  const float bottom = (float)height - kGroundH - kPipeGap * 0.5f - 12.0f;
  return (float)randRange((uint32_t)top, (uint32_t)bottom);
}

static void resetPipes(int16_t width, int16_t height) {
  for (uint8_t i = 0; i < kPipeCount; ++i) {
    s.pipes[i].x = (float)width + 44.0f + i * kPipeSpacing;
    s.pipes[i].gapY = randomGapY(height);
    s.pipes[i].scored = false;
  }
}

static void resetGame(int16_t width, int16_t height) {
  s.mode = s.micOk ? Mode::Running : Mode::NoMic;
  s.score = 0;
  s.displayDb = clampf(s.displayDb, kControlLowDb, kControlHighDb);
  s.birdY = yForDb(s.displayDb, height);
  s.previousBirdY = s.birdY;
  s.targetY = s.birdY;
  s.groundOffset = 0.0f;
  resetPipes(width, height);
  s.lastMs = millis();
  hal::display::markAllDirty();
}

static bool streamSink(const int16_t* samples, uint32_t count, void*) {
  if (!samples || count == 0) return true;
  for (uint32_t i = 0; i < count; ++i) {
    const int32_t v = samples[i];
    const int32_t a = v < 0 ? -v : v;
    s.squareSum += (uint64_t)((int64_t)v * (int64_t)v);
    if ((uint32_t)a > s.absPeak) s.absPeak = (uint32_t)a;
  }
  s.samplesInWindow += count;
  return true;
}

static void updateMic(uint32_t now, int16_t height) {
  if (!s.active || !s.micOk) return;
  s_microphone.poll();

  if ((uint32_t)(now - s.lastMicMs) < 50u) return;
  s.lastMicMs = now;

  if (s.samplesInWindow > 0) {
    const double meanSquare = (double)s.squareSum / (double)s.samplesInWindow;
    s.db = dbFromRms(sqrtf((float)meanSquare));
    s.peakDb = dbFromRms((float)s.absPeak);
    if(s.mode==Mode::Calibrating&&s.calibrationCount<64)
      s.calibrationWindows[s.calibrationCount++]=s.db;
    s.samplesInWindow = 0;
    s.squareSum = 0;
    s.absPeak = 0;
  }

  const float attack = s.db > s.displayDb ? 0.42f : 0.18f;
  s.displayDb += (s.db - s.displayDb) * attack;
  s.displayDb = clampf(s.displayDb, 18.0f, 112.0f);
  if(s.mode==Mode::Calibrating) {
    if(now-s.calibrationStarted>=2000&&s.calibrationCount>=20) {
      s.baselineDb=roomBaseline(s.calibrationWindows,s.calibrationCount);
      s.calibrated=true;s.displayDb=s.baselineDb;s.mode=s.afterCalibration;
      Serial.printf("[flappy] room baseline=%.1f windows=%u\n",s.baselineDb,s.calibrationCount);
    } else if(now-s.calibrationStarted>=5000) {
      s.mode=Mode::NoMic;
      Serial.println("[flappy] room calibration failed: insufficient audio");
    }
    hal::display::markAllDirty();
  }
  s.control = voiceControl(s.displayDb,s.baselineDb);
  s.targetY = yForDb(s.displayDb, height);
  snprintf(s.dbText, sizeof(s.dbText), "VOICE %u",(unsigned)(100*s.control));
}

static float rightmostPipeX() {
  float rightmost = s.pipes[0].x;
  for (uint8_t i = 1; i < kPipeCount; ++i) {
    if (s.pipes[i].x > rightmost) rightmost = s.pipes[i].x;
  }
  return rightmost;
}

static bool circleHitsRect(float cx, float cy, float radius, float x, float y, float w, float h) {
  const float nx = clampf(cx, x, x + w);
  const float ny = clampf(cy, y, y + h);
  const float dx = cx - nx;
  const float dy = cy - ny;
  return dx * dx + dy * dy <= radius * radius;
}

static void endGame() {
  if (s.score > s.highScore) s.highScore = s.score;
  s.mode = Mode::GameOver;
  Serial.printf("[flappy] game_over score=%u high=%u db=%.1f\n",
                s.score, s.highScore, s.displayDb);
  hal::display::markAllDirty();
}

static void updateGame(uint32_t now, int16_t width, int16_t height) {
  updateMic(now, height);

  uint32_t elapsed = now - s.lastMs;
  if (elapsed > kMaxElapsedMs) elapsed = kMaxElapsedMs;
  s.lastMs = now;

  if (s.mode != Mode::Running) return;

  const float dt = (float)elapsed / 1000.0f;
  s.previousBirdY = s.birdY;
  s.birdY += (s.targetY - s.birdY) * 0.16f;
  s.groundOffset += kPipeSpeed * dt;
  while (s.groundOffset >= 16.0f) s.groundOffset -= 16.0f;

  const float floorY = (float)height - kGroundH - kBirdRadius;
  if (s.birdY < kTopPad + kBirdRadius || s.birdY > floorY) {
    endGame();
    return;
  }

  for (uint8_t i = 0; i < kPipeCount; ++i) {
    Pipe& pipe = s.pipes[i];
    pipe.x -= kPipeSpeed * dt;

    if (!pipe.scored && pipe.x + kPipeW < kBirdX - kBirdRadius) {
      pipe.scored = true;
      if (s.score < 999) ++s.score;
    }

    if (pipe.x + kPipeW < -12.0f) {
      pipe.x = rightmostPipeX() + kPipeSpacing;
      pipe.gapY = randomGapY(height);
      pipe.scored = false;
    }

    const float topH = pipe.gapY - kPipeGap * 0.5f;
    const float bottomY = pipe.gapY + kPipeGap * 0.5f;
    const float bottomH = (float)height - kGroundH - bottomY;
    if (circleHitsRect(kBirdX, s.birdY, kBirdRadius, pipe.x, 0.0f, kPipeW, topH) ||
        circleHitsRect(kBirdX, s.birdY, kBirdRadius, pipe.x, bottomY, kPipeW, bottomH)) {
      endGame();
      return;
    }
  }

  hal::display::markAllDirty();
}

static uint16_t mix565(uint16_t a, uint16_t b, uint8_t amount) {
  const uint8_t ar = (uint8_t)((a >> 11) & 0x1F);
  const uint8_t ag = (uint8_t)((a >> 5) & 0x3F);
  const uint8_t ab = (uint8_t)(a & 0x1F);
  const uint8_t br = (uint8_t)((b >> 11) & 0x1F);
  const uint8_t bg = (uint8_t)((b >> 5) & 0x3F);
  const uint8_t bb = (uint8_t)(b & 0x1F);
  const uint8_t inv = (uint8_t)(255 - amount);
  const uint16_t r = (uint16_t)((ar * inv + br * amount) / 255u);
  const uint16_t g = (uint16_t)((ag * inv + bg * amount) / 255u);
  const uint16_t bl = (uint16_t)((ab * inv + bb * amount) / 255u);
  return (uint16_t)((r << 11) | (g << 5) | bl);
}

static void drawBackground(TFT_eSprite& canvas) {
  const int16_t w = canvas.width();
  const int16_t h = canvas.height();
  for (int16_t y = 0; y < h - (int16_t)kGroundH; y += 4) {
    const uint8_t mix = y<64?0:(uint8_t)(((y-64)*255)/(h-64));
    canvas.fillRect(0, y, w, 4, mix565(kSky, kSkyDark, mix));
  }

  canvas.fillCircle(26, 62, 10, kCloud);
  canvas.fillCircle(40, 56, 14, kCloud);
  canvas.fillCircle(58, 62, 11, kCloud);
  canvas.fillRect(26, 62, 34, 9, kCloud);

  canvas.fillCircle(158, 98, 8, kCloud);
  canvas.fillCircle(170, 93, 12, kCloud);
  canvas.fillCircle(185, 98, 9, kCloud);
  canvas.fillRect(158, 98, 28, 8, kCloud);

  const int16_t groundY = h - (int16_t)kGroundH;
  canvas.fillRect(0, groundY, w, (int16_t)kGroundH, kGround);
  canvas.drawFastHLine(0, groundY, w, kInk);
  for (int16_t x = -(int16_t)s.groundOffset; x < w; x += 16) {
    canvas.drawFastVLine(x, groundY + 5, 12, kGroundDark);
  }
}

static void drawPipe(TFT_eSprite& canvas, const Pipe& pipe) {
  const int16_t x = (int16_t)pipe.x;
  const int16_t gapTop = (int16_t)(pipe.gapY - kPipeGap * 0.5f);
  const int16_t gapBottom = (int16_t)(pipe.gapY + kPipeGap * 0.5f);
  const int16_t groundY = canvas.height() - (int16_t)kGroundH;

  if (x > canvas.width() || x + (int16_t)kPipeW < 0) return;

  canvas.fillRect(x, 0, (int16_t)kPipeW, gapTop, kPipe);
  canvas.fillRect(x - 3, gapTop - 10, (int16_t)kPipeW + 6, 10, kPipe);
  canvas.drawRect(x - 3, gapTop - 10, (int16_t)kPipeW + 6, 10, kInk);
  canvas.drawFastVLine(x + 5, 0, gapTop - 10, kPipeLight);
  canvas.drawFastVLine(x + (int16_t)kPipeW - 3, 0, gapTop - 10, kPipeDark);

  canvas.fillRect(x, gapBottom, (int16_t)kPipeW, groundY - gapBottom, kPipe);
  canvas.fillRect(x - 3, gapBottom, (int16_t)kPipeW + 6, 10, kPipe);
  canvas.drawRect(x - 3, gapBottom, (int16_t)kPipeW + 6, 10, kInk);
  canvas.drawFastVLine(x + 5, gapBottom + 10, groundY - gapBottom - 10, kPipeLight);
  canvas.drawFastVLine(x + (int16_t)kPipeW - 3, gapBottom + 10,
                       groundY - gapBottom - 10, kPipeDark);
}

static void drawBird(TFT_eSprite& canvas) {
  const int16_t x = (int16_t)kBirdX;
  const int16_t y = (int16_t)s.birdY;
  const float dy = s.birdY - s.previousBirdY;
  const int16_t wingY = (int16_t)clampf(2.0f - dy * 5.0f, -5.0f, 7.0f);

  canvas.fillCircle(x, y, (int16_t)kBirdRadius, kBird);
  canvas.drawCircle(x, y, (int16_t)kBirdRadius, kInk);
  canvas.fillCircle(x + 4, y - 4, 3, 0xFFFF);
  canvas.fillCircle(x + 5, y - 4, 1, kInk);
  canvas.fillTriangle(x + 8, y - 1, x + 20, y + 3, x + 8, y + 7, kBeak);
  canvas.drawTriangle(x + 8, y - 1, x + 20, y + 3, x + 8, y + 7, kInk);
  canvas.fillRoundRect(x - 12, y + wingY, 12, 7, 3, kBirdWing);
  canvas.drawRoundRect(x - 12, y + wingY, 12, 7, 3, kInk);
}

static void drawHud(TFT_eSprite& canvas) {
  char line[28];
  canvas.setTextFont(1); canvas.setTextSize(2);
  canvas.setTextColor(kInk, kSky);
  canvas.fillRect(0, ui::safe_area::contentTop(), canvas.width(), 21, kSky);
  snprintf(line, sizeof(line), "%u", s.score);
  ui::safe_area::drawString(canvas, line, 5, 2, 1);
  snprintf(line, sizeof(line), "HI %u", s.highScore);
  ui::safe_area::drawCentreString(canvas, line, canvas.width() / 2, 2, 1);
  canvas.setTextColor(kInk, kSky);
  ui::safe_area::drawRightString(canvas, s.dbText[0] ? s.dbText : "-- dB", canvas.width() - 5, 2, 1);

  const int16_t meterX = canvas.width() - 56;
  const int16_t meterY = ui::safe_area::contentTop()+23;
  canvas.drawRect(meterX, meterY, 50, 5, kInk);
  canvas.fillRect(meterX + 1, meterY + 1, (int16_t)(48.0f * s.control), 3, kWarn);
}

static void drawOverlay(TFT_eSprite& canvas) {
  canvas.setTextFont(1); canvas.setTextSize(2);
  if(s.mode==Mode::Ready||s.mode==Mode::Calibrating||s.mode==Mode::Paused) {
    canvas.fillRect(18,88,canvas.width()-36,112,kPanel);
    canvas.drawRect(18,88,canvas.width()-36,112,kWarn);
    canvas.setTextColor(kPanelText,kPanel);
    const bool measuring=s.mode==Mode::Calibrating;
    ui::safe_area::drawCentreString(canvas,measuring?"SETTING ROOM NOISE":s.mode==Mode::Ready?"ROOM NOISE SET":"PAUSED",canvas.width()/2,98, 1);
    ui::safe_area::drawCentreString(canvas,measuring?"Let the room sound normal":s.baselineDb>85?"Room loud: move closer":"Louder voice = fly higher",canvas.width()/2,124, 1);
    ui::safe_area::drawCentreString(canvas,measuring?"Stay quiet for 2 seconds":s.mode==Mode::Paused?"C: RESUME":"C: START",canvas.width()/2,149, 1);
    ui::safe_area::drawCentreString(canvas,measuring?"Game is stopped":"B: SET ROOM NOISE",canvas.width()/2,175, 1);
    return;
  }
  if (s.mode == Mode::NoMic) {
    canvas.fillRect(22, 96, canvas.width() - 44, 82, kPanel);
    canvas.drawRect(22, 96, canvas.width() - 44, 82, kWarn);
    canvas.setTextFont(1); canvas.setTextSize(2);
    canvas.setTextColor(kPanelText, kPanel);
    ui::safe_area::drawCentreString(canvas, "MIC UNAVAILABLE", canvas.width() / 2, 108, 1);
    ui::safe_area::drawCentreString(canvas, "A+B TO MENU", canvas.width() / 2, 136, 1);
    return;
  }

  if (s.mode == Mode::Paused) {
    canvas.fillRect(58, 112, 124, 48, kPanel);
    canvas.drawRect(58, 112, 124, 48, kWarn);
    canvas.setTextFont(1); canvas.setTextSize(2);
    canvas.setTextColor(kPanelText, kPanel);
    ui::safe_area::drawCentreString(canvas, "PAUSED", canvas.width() / 2, 122, 1);
    ui::safe_area::drawCentreString(canvas, "C: RESUME", canvas.width() / 2, 142, 1);
    return;
  }

  if (s.mode == Mode::GameOver) {
    char line[28];
    canvas.fillRect(34, 88, canvas.width() - 68, 104, kPanel);
    canvas.drawRect(34, 88, canvas.width() - 68, 104, kWarn);
    canvas.setTextFont(1); canvas.setTextSize(2);
    canvas.setTextColor(kWarn, kPanel);
    ui::safe_area::drawCentreString(canvas, "GAME OVER", canvas.width() / 2, 100, 1);
    canvas.setTextColor(kPanelText, kPanel);
    snprintf(line, sizeof(line), "SCORE %u", s.score);
    ui::safe_area::drawCentreString(canvas, line, canvas.width() / 2, 124, 1);
    snprintf(line, sizeof(line), "BEST %u", s.highScore);
    ui::safe_area::drawCentreString(canvas, line, canvas.width() / 2, 144, 1);
    ui::safe_area::drawCentreString(canvas, "C: PLAY   B: ROOM NOISE", canvas.width() / 2, 168, 1);
  }
}

}  // namespace

// Activation and resume retain the existing game reset and microphone recovery.
void enter() {
  const uint16_t high = s.highScore;
  memset(&s, 0, sizeof(s));
  s.highScore = high;
  s.active = true;
  s.displayDb = kControlLowDb;
  s.lastMicMs = millis();
  strlcpy(s.dbText, "-- dB", sizeof(s.dbText));
  s.micOk = s_microphone.open(streamSink, nullptr);
  if (!s.micOk) {
    Serial.println("[flappy] mic start failed");
  } else {
    Serial.println("[flappy] enter");
  }
  resetGame(kWidth,kHeight);
  if(s.micOk){s.mode=Mode::Ready;beginCalibration();}
}

void exitApp() {
  s_microphone.close();
  s.active = false;
  s.micOk = false;
  Serial.println("[flappy] exit");
}

bool isActive() {
  return s.active && s.micOk;
}

void tick() {
  updateGame(millis(), kWidth,kHeight);
}

void onInput(hal::input::InputEvent ev) {
  using ET = hal::input::EventType;

  if(ev.type==ET::ButtonB_Tap){beginCalibration();return;}
  if(s.mode==Mode::Calibrating||!s.calibrated)return;

  if (ev.type == ET::ButtonA_Tap || ev.type == ET::ButtonA_Press) {
    resetGame(kWidth,kHeight);
    return;
  }

  if (ev.type == ET::ButtonC_Tap && s.micOk) {
    if(s.mode==Mode::Ready||s.mode==Mode::GameOver)resetGame(kWidth,kHeight);
    else s.mode = s.mode == Mode::Paused ? Mode::Running : Mode::Paused;
    s.lastMs = millis();
    hal::display::markAllDirty();
  }
}

void onDraw(TFT_eSprite& canvas) {
  setSceneChromeBackground(kSky);
  drawBackground(canvas);
  for (uint8_t i = 0; i < kPipeCount; ++i) drawPipe(canvas, s.pipes[i]);
  drawBird(canvas);
  drawHud(canvas);
  drawOverlay(canvas);
  hal::display::markAllDirty();
}

#ifdef ORIGAMI_DEV_SERIAL_INPUT
void printStatus(Print& out) {
  const char* mode=s.mode==Mode::Ready?"ready":s.mode==Mode::Calibrating?"calibrating":s.mode==Mode::Paused?"paused":s.mode==Mode::Running?"running":s.mode==Mode::GameOver?"game-over":"no-mic";
  out.printf("[flappy] status mode=%s mic=%d baseline=%.2f calibrated=%d windows=%u db=%.2f control=%.3f y=%.2f pipe=%.2f score=%u\n",mode,s.micOk,s.baselineDb,s.calibrated,s.calibrationCount,s.displayDb,s.control,s.birdY,s.pipes[0].x,s.score);
}
void testNoise(Print& out) {
  unsigned failures=0;
  const float floors[]={35.0f,70.0f};
  for(float floor : floors) {
    if(voiceControl(floor,floor)!=0||voiceControl(floor+3,floor)!=0)++failures;
    if(fabsf(voiceControl(floor+15,floor)-0.5f)>.001f)++failures;
    if(voiceControl(floor+27,floor)!=1)++failures;
    float samples[]={floor,floor,floor,floor,floor,floor,floor,100};
    if(roomBaseline(samples,8)!=floor)++failures;
  }
  out.printf("[flappy] noise-test failures=%u quiet=35 loud=70\n",failures);
}
#endif

static void screenTick(uint32_t) {
  tick();
}

static void buildFlappyScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  scene.addRect("dumb-flappy.bg", {0, 0, width, height}, kSky, 0, 0);
  scene.addLabel("dumb-flappy.title", {0, 0, width, 20}, "Dumb Flappy Bird",
                 kPanelText, kPanel, 2, ui::scene::TextAlign::TopLeft, 1);
}

static const ::scene::Screen kScreenFlappy = {
    "Dumb Flappy Bird",
    nullptr,
    nullptr,
    screenTick,
    onInput,
    nullptr,
    onDraw,
    "game",
    buildFlappyScene,
};

static const ::scene::Screen* const kScreens[] = {&kScreenFlappy};
static core::health::RuntimePolicy runtimePolicy() {
    return core::health::whileBusy(isActive());
}
App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Dumb Flappy Bird", "dumb-flappy-bird", &kScreenFlappy,
                            enter, enter, exitApp, "src/apps/dumb_flappy_bird_app.cpp", kScreens, nullptr, runtimePolicy);

}  // namespace dumb_flappy_bird
}  // namespace apps

#endif  // MENU_APP
