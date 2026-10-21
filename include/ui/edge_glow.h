#pragma once

#include <stdint.h>

class TFT_eSPI;

namespace ui {

/**
 * Siri-style animated edge glow: a multicolour ring that follows the panel's
 * rounded corners and sweeps its hue around the screen. Stateless — the phase
 * comes from nowMs.
 *
 * A uniform-width rounded border on all four sides, full-saturation/full-
 * brightness. Draw the (transparent) status-bar content on top afterwards so the
 * colours run behind the clock.
 *
 * @param intensity mic level 0..255 — drives the ring *thickness* (a volume bar);
 *                  the colour stays vivid regardless.
 */
void paintEdgeGlow(TFT_eSPI& tft, uint32_t nowMs, uint8_t intensity = 255);

}  // namespace ui
