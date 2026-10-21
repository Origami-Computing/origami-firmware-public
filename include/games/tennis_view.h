#pragma once
/**
 * Tennis drawing: the pre-rendered stadium (tools/generate_tennis_art.py), the two players, the ball and the HUD,
 * straight into the 280x240 RGB565 canvas buffer (byte-swapped, as TFT_eSprite stores it), rows 38..239.
 *
 * Retained: each frame works out every item's box and a hash of what it shows (the ball, the two players, the
 * scoreboard, the message, the corner hint, the how-to-play card). Only the old and new boxes of items that changed get the background back and are
 * repainted, clipped (<= 8 merged rectangles), so a quiet frame repaints nothing and the display flushes only the
 * rows that changed. A full repaint every 10 s covers anything that drew over the canvas. No Arduino: host-tested in
 * test/test_tennis_view/test_host.cpp (incremental frames == full repaints, pixel for pixel).
 */
#include <stdint.h>

#include "core/tennis.h"
#include "games/dirty_rects.h"

namespace games {
namespace tennis_view {

using Rect = games::dirty::Rect;

struct Scene {
  uint32_t nowMs;
  uint32_t swingAt, aiSwingAt;               // racket animations: when the swing started (0 = none)
  core::tennis::Side swingSide, aiSwingSide;
  const char* message;                       // the message panel, or nullptr / "" for none
  const char* hint;                          // a small corner hint ("hold A: how to play"), or nullptr
  bool helpOpen;                             // the how-to-play card (games/tennis_help), on top of everything
  uint8_t helpPage;
};

// What the last frame drew. Zero it or invalidate() it when the canvas may have been drawn over (entry, resume).
struct Retained {
  static constexpr int kItems = 7, kMaxRects = 8;
  static constexpr uint32_t kFullMs = 10000;     // a full repaint now and then, in case anything drew over us
  Rect box[kItems];
  uint32_t sig[kItems];
  const uint16_t* canvas;
  uint32_t fullAt;
  bool valid;
  uint8_t rects;                                 // this frame's repainted rectangles (for the display's dirty rows)
  Rect rect[kMaxRects];
};
void invalidate(Retained& r);

// Optional draw-time split: pass a microsecond clock. measure = boxes and hashes, bg = restoring the background in
// the dirty rectangles, sprites = players and ball, hud = scoreboard and message. area = pixels repainted, of which
// spritePx / hudPx lie in the sprites' / HUD's boxes; rows = display rows to flush.
struct Timing {
  uint32_t (*clockUs)();
  uint32_t measureUs, bgUs, spritesUs, hudUs, area, spritePx, hudPx;
  uint16_t rows;
  uint8_t rects;
};

// r == nullptr: repaint everything.
void draw(uint16_t* canvas, const core::tennis::Game& g, const Scene& s, Retained* r = nullptr, Timing* t = nullptr);

}  // namespace tennis_view
}  // namespace games
