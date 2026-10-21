#pragma once
/**
 * Bowling drawing: the approach view (behind the bowler, the whole lane) and the pin-deck close-up, the ball, the
 * bowler's arms, the scorecard and banners. Draws straight into the 280x240 RGB565 canvas buffer (byte-swapped, as
 * TFT_eSprite stores it), rows 38..239. No Arduino: the host renders frames with it (test/test_bowling/render_host.cpp).
 */
#include <stdint.h>

#include "core/bowling.h"
#include "games/dirty_rects.h"

namespace games {
namespace bowling_view {

enum class View : uint8_t { Approach, Deck };
enum class Pose : uint8_t { Stance, Swing, FollowThrough };

struct Scene {
  View view;
  Pose pose;
  float armDeg;          // right arm: 0 = hanging down, + = back (towards the camera), - = forward
  bool ballInHand;
  uint32_t nowMs;
  uint32_t resultMs;     // how long the result banner has been up (0 = none)
  float speedMph;        // shown after a throw (0 = hide)
  const char* hint;      // bottom hint line, or nullptr
  const char* center;    // a centred message panel (calibration, errors), or nullptr
  int best;              // best score this session (game over screen)
  int8_t helpPage;       // the how-to-play panel: 0 = closed, else the page shown (1-based)
};

using Rect = games::dirty::Rect;   // half-open

// What the last frame drew, so the next one repaints only what changed (see bowling_view.cpp). Keep it with the game
// (PSRAM); zero it or invalidate() it when the canvas may have been drawn over (entry, resume).
struct Retained {
  static constexpr int kItems = 32, kMaxRects = 8;
  static constexpr uint32_t kFullMs = 10000;     // a full repaint now and then, in case anything drew over us
  static constexpr int kScratch = 200 * 40;      // banner text alpha map
  Rect box[kItems];                              // last frame: each item's footprint and parameter hash
  uint32_t sig[kItems];
  Rect cur[kItems];                              // this frame's, while it is measured
  uint32_t curSig[kItems];
  const uint16_t* canvas;
  uint32_t fullAt;
  uint8_t view;
  bool valid;
  uint8_t rects;                                 // this frame's repainted rectangles (for the display's dirty rows)
  Rect rect[kMaxRects];
  uint8_t scratch[kScratch];
};
void invalidate(Retained& r);

// The how-to-play panel's text must fit its card: the widest line in pixels, and the room there is.
constexpr int kHelpTextW = 252 - 24;
int helpWidestLine();
constexpr int kHelpFooterTop = 44 + 190 - 24;   // the page dots / "dial: page" row
int helpTextBottom();

// Optional draw-time split: pass a microsecond clock. measure = the bookkeeping pass, bg = restoring the background
// in the dirty rectangles, scene = lane/pins/ball/bowler, hud = scorecard/banners/text; area = pixels repainted.
struct Timing {
  uint32_t (*clockUs)();
  uint32_t measureUs, bgUs, sceneUs, hudUs, area, blends;   // blends = pixels painted over the background
  uint8_t rects;
};

// r == nullptr: repaint everything (banner text needs r's scratch; without it banners have no text).
void draw(uint16_t* canvas, const core::bowling::Match& m, const Scene& s, Retained* r = nullptr, Timing* t = nullptr);

}  // namespace bowling_view
}  // namespace games
