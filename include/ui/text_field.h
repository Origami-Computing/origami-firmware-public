#pragma once

#include <TB_TFT_eSPI.h>
#include <stdint.h>
#include <string.h>
#include "ui/safe_area_draw.h"

namespace ui {

/**
 * Flicker-free fixed-position text widget for direct-panel apps (e.g. MP3 now-playing,
 * voice recorder, system status bar).
 *
 * Why: `fillRect(...) + drawString(...)` flashes on every redraw — the wipe paints the
 * background for one frame before the glyph paints over it. TFT_eSPI's `drawString`
 * already fills each glyph cell with the configured background colour, so for a
 * **fixed-width format string** (e.g. "MM:SS", "HH:MM") subsequent updates can replace
 * the previous text in place without any wipe at all.
 *
 * Usage:
 *   static ui::TextField s_timer;
 *   s_timer.style(1, 4, TC_DATUM, kUiFg, kUiBg).at(sw/2, y);
 *   ...
 *   s_timer.draw(tft, "00:42");       // only pushes pixels when the string changed
 *   s_timer.invalidate();             // force next draw to push (e.g. after fillScreen)
 *
 * Constraint: only flicker-free when every subsequent string is **at least as wide**
 * as the previous one. For monospace formats this is guaranteed. If width can shrink,
 * call `invalidate()` and ensure the area behind has been wiped first.
 */
class TextField {
 public:
  TextField& style(uint8_t font, uint8_t size, uint8_t datum,
                   uint16_t fg, uint16_t bg) {
    font_  = font;
    size_  = size;
    datum_ = datum;
    fg_    = fg;
    bg_    = bg;
    dirty_ = true;
    return *this;
  }

  TextField& at(int x, int y) {
    if (x != x_ || y != y_) dirty_ = true;
    x_ = x;
    y_ = y;
    return *this;
  }

  /** Force `draw()` to push pixels on its next call even if the string hasn't changed. */
  void invalidate() {
    dirty_ = true;
    last_[0] = '\0';
  }

  /** Returns true if the panel was actually written to. */
  bool draw(TFT_eSPI& tft, const char* text) {
    if (!text) text = "";
    if (!dirty_ && strncmp(text, last_, sizeof(last_) - 1) == 0) return false;
    tft.setTextFont(font_);
    tft.setTextSize(size_);
    tft.setTextDatum(datum_);
    tft.setTextColor(fg_, bg_);
    tft.setTextWrap(false, false);
    // The system clock is the sole field intentionally in the status strip.
    const auto region = y_ < safe_area::contentTop() ? safe_area::Region::Screen : safe_area::Region::Content;
    safe_area::drawString(tft, text, x_, y_, font_, region);
    // Restore neutral defaults so shared `tft` state can't leak font/size into the next caller.
    tft.setTextDatum(TL_DATUM);
    tft.setTextSize(1);
    strncpy(last_, text, sizeof(last_) - 1);
    last_[sizeof(last_) - 1] = '\0';
    dirty_ = false;
    return true;
  }

 private:
  int x_ = 0;
  int y_ = 0;
  uint8_t font_  = 1;
  uint8_t size_  = 1;
  uint8_t datum_ = TL_DATUM;
  uint16_t fg_   = 0xFFFF;
  uint16_t bg_   = 0x0000;
  char last_[24] = {0};
  bool dirty_   = true;
};

}  // namespace ui
