#include "config/screen_chrome_layout.h"
/** Steps workflow and view; services::steps owns the pedometer and totals. */
#include "apps/steps_app.h"

#include <Arduino.h>
#include "app.h"
#include "services/steps.h"
#include <stdio.h>

#include "config/steps_layout.h"
#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "scene/screen.h"
#include "ui/scene.h"

namespace apps {
namespace steps {

namespace {

constexpr uint32_t kForegroundRefreshMs = 500;
constexpr uint16_t kDailyGoalDefault = 6000;
static bool s_active = false;
static uint32_t s_lastDrawMs = 0;
static uint32_t s_lastDrawnVal = 0xFFFFFFFFu;
static bool s_needFullRedraw = false;

// ----- drawing ---------------------------------------------------------------

static void setVisible(ui::scene::Node* node, bool visible) {
  if (node) node->visible = visible;
}

static void buildStepsScene(ui::scene::Scene& scene, int16_t, int16_t, uint32_t total) {
  const auto state = services::steps::snapshot();
  const uint32_t goal = state.dailyGoal == 0 ? kDailyGoalDefault : state.dailyGoal;
  const uint32_t pct  = total >= goal ? 100u : (total * 100u) / goal;

  scene.clear();

  using namespace steps_layout;

  setVisible(scene.addRect("content.bg", {kContentBgX, kContentBgY, kContentBgW, kContentBgH},
                           kContentBgFillColor, kContentBgRadius, kContentBgZ),
             kContentBgVisible);

  char totalBuf[16];
  snprintf(totalBuf, sizeof(totalBuf), "%lu", (unsigned long)total);
  setVisible(scene.addLabel("steps.total.value",
                            {kStepsTotalValueX, kStepsTotalValueY, kStepsTotalValueW, kStepsTotalValueH},
                            totalBuf, kStepsTotalValueFgColor, kStepsTotalValueBgColor,
                            kStepsTotalValueTextSize, ui::scene::TextAlign::MiddleCenter, kStepsTotalValueZ),
             kStepsTotalValueVisible);
  setVisible(scene.addLabel("steps.total.caption",
                            {kStepsTotalCaptionX, kStepsTotalCaptionY, kStepsTotalCaptionW, kStepsTotalCaptionH},
                            kStepsTotalCaptionText, kStepsTotalCaptionFgColor, kStepsTotalCaptionBgColor,
                            kStepsTotalCaptionTextSize, ui::scene::TextAlign::TopCenter, kStepsTotalCaptionZ),
             kStepsTotalCaptionVisible);
  setVisible(scene.addProgressBar("steps.progress",
                                  {kStepsProgressX, kStepsProgressY, kStepsProgressW, kStepsProgressH},
                                  total, goal, kStepsProgressTrackColor, kStepsProgressFillColor,
                                  kStepsProgressRadius, kStepsProgressZ),
             kStepsProgressVisible);

  char goalBuf[48];
  snprintf(goalBuf, sizeof(goalBuf), "Goal %lu  -  %lu%%",
           (unsigned long)goal, (unsigned long)pct);
  setVisible(scene.addLabel("steps.goal", {kStepsGoalX, kStepsGoalY, kStepsGoalW, kStepsGoalH},
                            goalBuf, kStepsGoalFgColor, kStepsGoalBgColor,
                            kStepsGoalTextSize, ui::scene::TextAlign::TopCenter, kStepsGoalZ),
             kStepsGoalVisible);

  char ydayBuf[40];
  snprintf(ydayBuf, sizeof(ydayBuf),
           state.yesterday > 0 ? "Yesterday: %lu" : "Yesterday: --",
           (unsigned long)state.yesterday);
  setVisible(scene.addLabel("steps.yesterday",
                            {kStepsYesterdayX, kStepsYesterdayY, kStepsYesterdayW, kStepsYesterdayH},
                            ydayBuf, kStepsYesterdayFgColor, kStepsYesterdayBgColor,
                            kStepsYesterdayTextSize, ui::scene::TextAlign::TopCenter, kStepsYesterdayZ),
             kStepsYesterdayVisible);

}

static void buildCurrentStepsScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  buildStepsScene(scene, width, height, services::steps::snapshot().today);
}

static void drawScreen(TFT_eSprite& canvas, uint32_t total) {
  ui::scene::Scene& scene = ui::scene::scratchScene();
  buildStepsScene(scene, canvas.width(), canvas.height(), total);
  scene.render(canvas);
  hal::display::markAllDirty();
}

}  // namespace

// -----------------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------------

static void prepareWorkflow() {
  s_active = true;
  services::steps::poll();
}

static void enterView() {
  s_needFullRedraw = true;
  s_lastDrawnVal = 0xFFFFFFFFu;
  hal::display::markAllDirty();
}

static void exitApp() {
  services::steps::flushDirty();
  s_active = false;
}

void tick() {
  services::steps::poll();
}

void onInput(hal::input::InputEvent ev) {
  using ET = hal::input::EventType;
  if (ev.type == ET::ButtonA_Press) {
    services::steps::resetToday();
    s_needFullRedraw = true;
    hal::display::markAllDirty();
  } else if (ev.type == ET::Scroll && ev.delta != 0) {
    const uint16_t dailyGoal = services::steps::snapshot().dailyGoal;
    int32_t goal = (int32_t)dailyGoal + (int32_t)ev.delta * 500;
    if (goal < 1000)  goal = 1000;
    if (goal > 30000) goal = 30000;
    const uint16_t newGoal = (uint16_t)goal;
    if (newGoal != dailyGoal) {
      services::steps::setDailyGoal(newGoal);
      s_needFullRedraw = true;
      hal::display::markAllDirty();
    }
  }
}

void onDraw(TFT_eSprite& canvas) {
  if (!s_active) return;
  const uint32_t now = millis();
  const uint32_t today = services::steps::snapshot().today;
  const bool stale = (uint32_t)(now - s_lastDrawMs) >= kForegroundRefreshMs &&
                     today != s_lastDrawnVal;
  if (!s_needFullRedraw && !stale) return;
  drawScreen(canvas, today);
  s_lastDrawnVal   = today;
  s_lastDrawMs     = now;
  s_needFullRedraw = false;
}

static void screenTick(uint32_t) {
  tick();
}

const ::scene::Screen kScreenSteps = {
    "Steps",
    enterView,
    nullptr,
    screenTick,
    onInput,
    nullptr,
    onDraw,
    "main",
    buildCurrentStepsScene,
    &screen_chrome_layout::kStepsMainTopBarColor,
};

static const ::scene::Screen* const kScreens[] = {&kScreenSteps};
App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Steps", "steps", &kScreenSteps, prepareWorkflow, prepareWorkflow, exitApp,
                            "src/apps/steps_app.cpp", kScreens, nullptr, nullptr);

}  // namespace steps
}  // namespace apps
