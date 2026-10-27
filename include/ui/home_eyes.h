#pragma once

#include <stdint.h>

#include "config/app_config.h"

class TFT_eSPI;
class TFT_eSprite;

namespace ui {
namespace home_eyes {

constexpr int kShellScalePercent = 85;
constexpr int kCanonicalInkWidth = 230 * kShellScalePercent / 100;

/** Same fields as web KamiEyes `EyeOcc` (normalized top/bottom lid occluders). */
struct KamiEyeOcc {
  float topY;
  float topX;
  float bottomY;
  float bottomX;
  float size;
};

/** Default rest pose: lids fully retracted. Set bottomY > 0 to raise a bottom lid. */
inline KamiEyeOcc kamiNeutralOcc() {
  KamiEyeOcc z;
  z.topY = z.topX = z.bottomX = z.bottomY = 0.f;
  z.size                                  = 100.f;
  return z;
}

/** Legacy tuning storage retained for callers; the shared rig uses web presets and IMU gaze. */
struct KamiAliveTune {
  bool enabled;
  /** Minimum interval between alive updates (ms). */
  uint16_t period_ms;
  float top_y_amp;
  float bottom_y_amp;
  float top_x_amp;
  float bottom_x_amp;
  /** Extra scale wobble around base size (0.07 ≈ ±7%). */
  float size_rel_amp;
  /** Multiplier on time for phase accumulation (~1.0 matches legacy feel). */
  float phase_speed;
};

inline KamiAliveTune defaultKamiAliveTune() {
  KamiAliveTune t{};
  t.enabled        = false;
  t.period_ms      = 900;
  t.top_y_amp      = 0.015f;
  t.bottom_y_amp   = 0.018f;
  t.top_x_amp      = 0.02f;
  t.bottom_x_amp   = 0.018f;
  t.size_rel_amp   = 0.025f;
  t.phase_speed    = 1.0f;
  return t;
}

void setKamiAliveTune(const KamiAliveTune& tune);
KamiAliveTune getKamiAliveTune();

/**
 * Physics-driven subpixel gaze offset applied by the home renderer.
 * Call once per animation tick (from agent_render::serviceEyesShellTick) before
 * drawFrame / serviceEyesShell. Resets to (0,0) on endSession().
 */
void setEyeOffset(float x, float y);
void setHomeMotion(float squeeze, bool contact);
// Centered conversation actor uses the same cached geometry and natural blink.
bool drawConversationEyes(TFT_eSPI& tft, float x, float y, uint32_t now, bool onlyChanged = false);
// Pure UI paint using the wake face/hand, starting open before the happy wave.
// Does not start a wake action or mutate home expression/blink state.
void drawOutroWave(TFT_eSPI& target,uint32_t age);
bool drawTravelEyes(TFT_eSPI& tft, float avatar, float offsetX=0, float offsetY=0);
bool drawThinkingEyes(TFT_eSPI& tft, float settle, uint32_t now, bool onlyChanged);
bool setEmotion(const char* name, bool immediate = false);
void sleepTransition(TFT_eSPI& tft);
bool handleDebugCommand(const char* line);

/** Transition with the web emotion curve, evaluated each rendered frame. */
void setKamiEmotionTargets(const KamiEyeOcc& left, const KamiEyeOcc& right, float springSpeed = 0.8f);

/**
 * Load left/right occluder targets (and optional spring + alive tuning) from a JSON file on SD.
 * Schema: { "left":{...}, "right":{...}, "spring":0.8, "alive":{...} } — same field names as KamiEyeOcc + nested alive tune.
 * Returns false if the file is missing or invalid (caller keeps current pose).
 */
bool loadKamiEmotionFromJsonFile(const char* path);

/** Apply an optional JSON preset or the canonical thinking expression. */
void beginThinkingEmotion(const char* jsonPath);
void endThinkingEmotion();

/** Advance the current emotion transition and render open eyes. */
bool advanceOpenEyeOverlay(TFT_eSPI& tft, int yOffset, uint32_t nowMs);

// Homescreen eye strip timing (legacy: per-frame table; shell blink uses serviceShellBlink()).
constexpr int kSequenceLen = 4;
extern const uint8_t kFrameSequence[kSequenceLen];
extern const uint16_t kFrameDurationsMs[HOME_SCREEN_NUM_FRAMES];

/** Web cadence: 3.5–5.5s rest, 60ms closing and 140ms reopening. */
void resetShellBlink();
/** Pause blink advances (e.g. while A+B held on Kami for PTT) so SPI work does not race alloc/mic. */
void setShellBlinkFrozen(bool frozen);
/** True once when the normal shell blink sequence returns to open eyes. */
bool takeShellBlinkCycleComplete();
/** Request a short held half-blink on the next shell service tick. */
void requestShellBlinkHold(uint32_t holdMs);
bool serviceShellBlink(TFT_eSPI& tft);

/** Enable/disable IMU tap-to-hearts animation. Disabled by default. */
void setTapHeartsEnabled(bool en);
bool getTapHeartsEnabled();

/** Continuous blink + IMU gaze/tap + antialiased vector eyelid masks. */
bool serviceEyesShell(TFT_eSPI& tft, int yOffset = 0);

/**
 * Draw the canonical eye geometry into a small avatar sprite. Legacy frame
 * indexes map to open, half-closed and closed poses; no bitmap is loaded.
 */
bool drawMiniFrame(TFT_eSprite& dst, int frameIndex, uint16_t bgColor);

/** Draw the current pose at avatar scale. */
bool drawMiniFrameToPanel(TFT_eSPI& tft, int frameIndex, int x, int y, int w, int h, uint16_t bgColor);

/** Draw the current home backing rectangle at a new size, including gaze. */
bool drawFrameToPanel(TFT_eSPI& tft, int frameIndex, int x, int y, int w, int h, uint16_t bgColor);

// Session initialization is allocation-free and does not access SD.
bool beginSession(TFT_eSPI& tft);
/** Compatibility alias: the new rig has no separate single-frame cache. */
bool beginSessionSingleFrame(TFT_eSPI& tft);
/** Compatibility alias: all poses are available immediately. */
bool upgradeToFullSession(TFT_eSPI& tft);
void endSession();

bool isLoaded();
bool isProcedural();
int numFrames();

/** Current on-panel frame bounds for editor/diagnostics. Returns false before a session is loaded. */
bool currentFrameBounds(TFT_eSPI& tft, int* x, int* y, int* w, int* h);

/** Call before first procedural draw when yOffset != 0 or after a context switch. */
void resetProceduralPaintState(int yOffset = 0);

/** Compatibility frame index: 0=open, 1=half-closed, 2=closed. */
bool drawFrame(TFT_eSPI& tft, int frameIndex, int yOffset = 0);

/** If the last drawFrame() overwrote the top status strip, return true once (menu repaints chrome). */
bool takeEyeDrawChromeDirty();

/** White band behind procedural eyes when composited on a non-white UI (e.g. Kami). */
void fillEyeCanvasBackground(TFT_eSPI& tft, int yOffset, int screenW, int bandH);

/** After first BMP draw: fill any gap below the clipped BMP if the panel is taller than the asset. */
void fillBmpBottomLetterbox(TFT_eSPI& tft);

/**
 * Use after advanceOpenEyeOverlay() returns true: repaints top/bottom letterbox bands only when the last
 * advance drew the full eye BMP band. Skips redundant full-width fills after partial occluder-only updates.
 */
void fillBmpBottomLetterboxAfterAdvance(TFT_eSPI& tft);

}  // namespace home_eyes
}  // namespace ui
