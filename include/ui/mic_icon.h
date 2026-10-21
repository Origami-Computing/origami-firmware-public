#pragma once

#include <TB_TFT_eSPI.h>
#include <stdint.h>

namespace ui {

/**
 * Pebble-style microphone icon with animated level fill.
 *
 * Layout (all measurements in pixels):
 *
 *      ┌──────┐
 *      │      │   capsule — double-stroke rounded rect
 *      │██████│   fill rises from bottom proportional to `level`
 *      └──────┘
 *        │  │
 *        ╰──╯     U-shaped stand (two parallel vertical lines + arc)
 *          │
 *        ─────    base (two horizontal lines)
 *
 * Callers are responsible for tracking when the outline is stale and calling
 * drawMicIconFrame().  drawMicIconLevel() only repaints the capsule fill, which
 * is cheap enough to call every level tick (no full-glyph wipe).
 */

/** Total height of the glyph from capsule top to base bottom, in pixels. */
int micIconHeight();

/**
 * Draw the mic capsule outline + stand + base.
 * `cx` is the horizontal center; `cy` is the top edge of the capsule.
 * `active`: true = normal foreground colour; false = muted colour.
 * Wipes the bounding rect with kUiBg before drawing so old glyphs are erased.
 */
void drawMicIconFrame(TFT_eSPI& tft, int cx, int cy, bool active);

/**
 * Repaint only the capsule fill (the animated level bar inside the rounded rect).
 * `active`: true = coloured fill (black→red with level); false = kUiPanel solid fill.
 * `level`: 0–255 fill fraction.
 * Call after drawMicIconFrame() — does not wipe the outline.
 */
void drawMicIconLevel(TFT_eSPI& tft, int cx, int cy, bool active, uint8_t level);

}  // namespace ui
