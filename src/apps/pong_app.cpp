/**
 * @file pong_app.cpp
 * @brief Single-player Pong with rotary paddle control and a capped-speed AI opponent.
 */

#ifdef MENU_APP

#include "apps/pong_app.h"
#include "ui/safe_area_draw.h"
#include "ui/screen_chrome.h"

#include <Arduino.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "hal/hal_display.h"
#include "scene/screen.h"
#include "ui/scene.h"

namespace apps {
namespace pong {

static constexpr uint16_t kBg = 0x0000;
static constexpr uint16_t kCourt = 0x39E7;
static constexpr uint16_t kDim = 0x18E3;
static constexpr uint16_t kInk = 0xFFFF;
static constexpr uint16_t kAccent = 0x07FF;
static constexpr uint16_t kWarn = 0xFBE0;

static constexpr float kPaddleW = 8.0f;
static constexpr float kPaddleH = 46.0f;
static constexpr float kPaddleMargin = 20.0f;
static constexpr float kBallSize = 7.0f;
static constexpr float kPlayerStep = 10.5f;
static constexpr float kPlayfieldTop = ui::safe_area::contentTop()+52.0f;
static constexpr float kPlayfieldBottomPad = 16.0f;
static constexpr float kBaseSpeed = 126.0f;
static constexpr float kMaxSpeed = 224.0f;
static constexpr float kAiSpeed = 118.0f;
static constexpr float kAiDeadband = 6.0f;
static constexpr uint8_t kWinningScore = 7;

struct State {
  float playerY;
  float aiY;
  float ballX;
  float ballY;
  float ballVx;
  float ballVy;
  float ballSpeed;
  int8_t servingDir;
  uint8_t playerScore;
  uint8_t aiScore;
  uint32_t lastMs;
  bool waitingServe;
  bool paused;
  bool matchOver;
};

static State s;

static float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static float paddleMaxY(int16_t height) {
  return (float)height - kPaddleH - kPlayfieldBottomPad;
}

static void centerPaddles(int16_t height) {
  const float y = (kPlayfieldTop+(float)height-kPlayfieldBottomPad-kPaddleH)*0.5f;
  s.playerY = y;
  s.aiY = y;
}

static void holdBallForServe(int16_t width, int16_t height, int8_t dir) {
  s.servingDir = dir < 0 ? -1 : 1;
  s.ballX = (float)width * 0.5f - kBallSize * 0.5f;
  s.ballY = (kPlayfieldTop+(float)height-kPlayfieldBottomPad-kBallSize)*0.5f;
  s.ballVx = 0.0f;
  s.ballVy = 0.0f;
  s.ballSpeed = kBaseSpeed;
  s.waitingServe = true;
}

static void resetMatch(int16_t width, int16_t height) {
  memset(&s, 0, sizeof(s));
  centerPaddles(height);
  s.lastMs = millis();
  holdBallForServe(width, height, 1);
}

static void serveBall() {
  if (s.matchOver) {
    resetMatch(ui::safe_area::kWidth, ui::safe_area::kHeight);
    return;
  }
  if (!s.waitingServe) return;

  const float angle = ((int32_t)(esp_random() % 70u) - 35) * 3.14159265f / 180.0f;
  s.ballSpeed = kBaseSpeed;
  s.ballVx = cosf(angle) * s.ballSpeed * (float)s.servingDir;
  s.ballVy = sinf(angle) * s.ballSpeed;
  if (fabsf(s.ballVy) < 28.0f) s.ballVy = s.ballVy < 0.0f ? -28.0f : 28.0f;
  s.waitingServe = false;
  s.paused = false;
  s.lastMs = millis();
}

static void awardPoint(bool playerScored, int16_t width, int16_t height) {
  if (playerScored) {
    if (s.playerScore < 99) s.playerScore++;
  } else if (s.aiScore < 99) {
    s.aiScore++;
  }

  s.matchOver = s.playerScore >= kWinningScore || s.aiScore >= kWinningScore;
  centerPaddles(height);
  holdBallForServe(width, height, playerScored ? -1 : 1);
}

static void bounceFromPaddle(float paddleY, bool playerPaddle) {
  const float paddleCenter = paddleY + kPaddleH * 0.5f;
  const float ballCenter = s.ballY + kBallSize * 0.5f;
  const float offset = clampf((ballCenter - paddleCenter) / (kPaddleH * 0.5f), -1.0f, 1.0f);
  s.ballSpeed = clampf(s.ballSpeed + 11.0f, kBaseSpeed, kMaxSpeed);
  s.ballVx = (playerPaddle ? 1.0f : -1.0f) * s.ballSpeed;
  s.ballVy = offset * 118.0f;
  if (fabsf(s.ballVy) < 24.0f) s.ballVy = offset < 0.0f ? -24.0f : 24.0f;
}

static void bounceOffHorizontalWalls(int16_t height) {
  const float minY = kPlayfieldTop;
  const float maxY = (float)height - kPlayfieldBottomPad - kBallSize;

  for (uint8_t i = 0; i < 2; ++i) {
    if (s.ballY < minY) {
      s.ballY = minY + (minY - s.ballY);
      s.ballVy = fabsf(s.ballVy);
    } else if (s.ballY > maxY) {
      s.ballY = maxY - (s.ballY - maxY);
      s.ballVy = -fabsf(s.ballVy);
    } else {
      break;
    }
  }

  s.ballY = clampf(s.ballY, minY, maxY);
}

static void updateGame(uint32_t now, int16_t width, int16_t height) {
  uint32_t elapsed = now - s.lastMs;
  if (elapsed > 50u) elapsed = 50u;
  s.lastMs = now;
  if (s.waitingServe || s.paused || s.matchOver) return;

  const float dt = (float)elapsed / 1000.0f;
  const float aiTarget = s.ballY + kBallSize * 0.5f - kPaddleH * 0.5f;
  const float aiDelta = aiTarget - s.aiY;
  if (fabsf(aiDelta) > kAiDeadband) {
    s.aiY += clampf(aiDelta, -kAiSpeed * dt, kAiSpeed * dt);
    s.aiY = clampf(s.aiY, kPlayfieldTop, paddleMaxY(height));
  }

  s.ballX += s.ballVx * dt;
  s.ballY += s.ballVy * dt;
  bounceOffHorizontalWalls(height);

  const float playerX = kPaddleMargin;
  const float aiX = (float)width - kPaddleMargin - kPaddleW;
  const float ballMidY = s.ballY + kBallSize * 0.5f;

  if (s.ballVx < 0.0f &&
      s.ballX <= playerX + kPaddleW &&
      s.ballX + kBallSize >= playerX &&
      ballMidY >= s.playerY &&
      ballMidY <= s.playerY + kPaddleH) {
    s.ballX = playerX + kPaddleW;
    bounceFromPaddle(s.playerY, true);
  }

  if (s.ballVx > 0.0f &&
      s.ballX + kBallSize >= aiX &&
      s.ballX <= aiX + kPaddleW &&
      ballMidY >= s.aiY &&
      ballMidY <= s.aiY + kPaddleH) {
    s.ballX = aiX - kBallSize;
    bounceFromPaddle(s.aiY, false);
  }

  if (s.ballX < -16.0f) {
    awardPoint(false, width, height);
  } else if (s.ballX > width + 16.0f) {
    awardPoint(true, width, height);
  }
}

static void prepareWorkflow() {
  resetMatch(ui::safe_area::kWidth, ui::safe_area::kHeight);
  Serial.println("[pong] enter");
}

static void exitApp() {
  Serial.println("[pong] exit");
}

void tick() {
  updateGame(millis(), ui::safe_area::kWidth, ui::safe_area::kHeight);
}

static void drawCourt(TFT_eSprite& canvas) {
  hal::display::clearContentArea(kBg);
  const int16_t width = canvas.width();
  const int16_t height = canvas.height();

  canvas.drawRect(18, int(kPlayfieldTop), width-36, height-int(kPlayfieldTop+kPlayfieldBottomPad), kDim);
  for (int16_t y = int(kPlayfieldTop); y < height - int(kPlayfieldBottomPad); y += 16) {
    canvas.drawFastVLine(width / 2, y, 8, kCourt);
  }
}

static void drawScore(TFT_eSprite& canvas) {
  char left[4];
  char right[4];
  snprintf(left, sizeof(left), "%u", s.playerScore);
  snprintf(right, sizeof(right), "%u", s.aiScore);

  canvas.setTextFont(1); canvas.setTextSize(3);
  canvas.setTextColor(kCourt, kBg);
  ui::safe_area::drawRightString(canvas, left, canvas.width() / 2 - 18, ui::safe_area::contentTop()+24, 1);
  ui::safe_area::drawString(canvas, right, canvas.width() / 2 + 18, ui::safe_area::contentTop()+24, 1);

  canvas.setTextFont(1); canvas.setTextSize(2);
  canvas.setTextColor(kInk, kBg);
  ui::safe_area::drawString(canvas, "PONG", 5, 2, 1);
  canvas.setTextColor(kAccent, kBg);
  ui::safe_area::drawRightString(canvas, "1P", canvas.width() - 5, 2, 1);
}

static void drawOverlay(TFT_eSprite& canvas) {
  canvas.setTextFont(1); canvas.setTextSize(2);
  if (s.matchOver) {
    canvas.fillRect(36, 112, 168, 56, kBg);
    canvas.drawRect(36, 112, 168, 56, kWarn);
    canvas.setTextColor(kWarn, kBg);
    ui::safe_area::drawCentreString(canvas, s.playerScore > s.aiScore ? "YOU WIN" : "AI WINS",
                            canvas.width() / 2, 119, 1);
    canvas.setTextColor(kInk, kBg);
    ui::safe_area::drawCentreString(canvas, "B: NEW MATCH", canvas.width() / 2, 143, 1);
    return;
  }

  if (s.waitingServe) {
    canvas.fillRect(50, 116, 140, 42, kBg);
    canvas.drawRect(50, 116, 140, 42, kDim);
    canvas.setTextColor(kInk, kBg);
    ui::safe_area::drawCentreString(canvas, "B: SERVE", canvas.width() / 2, 128, 1);
    return;
  }

  if (s.paused) {
    canvas.fillRect(66, 120, 108, 34, kBg);
    canvas.drawRect(66, 120, 108, 34, kDim);
    canvas.setTextColor(kInk, kBg);
    ui::safe_area::drawCentreString(canvas, "PAUSED", canvas.width() / 2, 128, 1);
  }
}

void onDraw(TFT_eSprite& canvas) {
  setSceneChromeBackground(kBg);
  drawCourt(canvas);
  drawScore(canvas);

  const int16_t width = canvas.width();
  const float playerX = kPaddleMargin;
  const float aiX = (float)width - kPaddleMargin - kPaddleW;

  canvas.fillRect((int16_t)playerX, (int16_t)s.playerY,
                  (int16_t)kPaddleW, (int16_t)kPaddleH, kInk);
  canvas.fillRect((int16_t)aiX, (int16_t)s.aiY,
                  (int16_t)kPaddleW, (int16_t)kPaddleH, kCourt);
  canvas.fillRect((int16_t)s.ballX, (int16_t)s.ballY,
                  (int16_t)kBallSize, (int16_t)kBallSize, kInk);

  drawOverlay(canvas);
  hal::display::markAllDirty();
}

void onInput(hal::input::InputEvent ev) {
  using ET = hal::input::EventType;

  if (ev.type == ET::Scroll && ev.delta != 0) {
    s.playerY = clampf(s.playerY + ev.delta * kPlayerStep, kPlayfieldTop, paddleMaxY(ui::safe_area::kHeight));
    return;
  }

  if (ev.type == ET::ButtonB_Tap || ev.type == ET::ButtonB_Press) {
    serveBall();
    return;
  }

  if (ev.type == ET::ButtonA_Tap || ev.type == ET::ButtonA_Press) {
    resetMatch(ui::safe_area::kWidth, ui::safe_area::kHeight);
    return;
  }

  if (ev.type == ET::ButtonC_Tap && !s.waitingServe && !s.matchOver) {
    s.paused = !s.paused;
    s.lastMs = millis();
    return;
  }
}

static void screenTick(uint32_t) {
  tick();
}

static void buildPongScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  scene.addRect("pong.bg", {0, 0, width, height}, kBg, 0, 0);
  scene.addLabel("pong.title", {4, 0, 80, 18}, "PONG",
                 kInk, kBg, 2, ui::scene::TextAlign::TopLeft, 1);
  scene.addRect("pong.net", {(int16_t)(width / 2), 26, 1, (int16_t)(height - 38)},
                kCourt, 0, 1);
}

static const ::scene::Screen kScreenPong = {
    "Pong",
    nullptr,
    nullptr,
    screenTick,
    onInput,
    nullptr,
    onDraw,
    "game",
    buildPongScene,
};

static const ::scene::Screen* const kScreens[] = {&kScreenPong};
App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Pong", "pong", &kScreenPong, prepareWorkflow, prepareWorkflow, exitApp,
                            "src/apps/pong_app.cpp", kScreens, nullptr, nullptr);

}  // namespace pong
}  // namespace apps

#endif  // MENU_APP
