#pragma once

#include <TB_TFT_eSPI.h>

namespace ui {

/**
 * Animated recording-level meter: N sine-modulated bars + elapsed-time label.
 *
 * Owns one TFT_eSprite for double-buffered bar rendering; falls back to direct
 * TFT writes when sprite allocation fails (2 MB PSRAM modules under pressure).
 *
 * Usage:
 *   static ui::RecordingMeter s_meter;
 *   // per frame, while recording:
 *   s_meter.draw(tft, cx, barCenterY, phase++, elapsedSeconds);
 *   // on exit / state change:
 *   s_meter.release();
 */
struct RecordingMeter {
    static constexpr int kBarCount = 11;
    static constexpr int kBarW     = 10;
    static constexpr int kBarGap   = 4;
    static constexpr int kBandH    = 92;  // total height of the bar band

    /** Render bars + timer into sprite and push to TFT (or fall back to direct).
     *  @param cx          Horizontal centre of the meter.
     *  @param barCenterY  Y coordinate of the bar vertical midpoint.
     *  @param phase       Monotonically increasing animation counter (caller increments).
     *  @param seconds     Elapsed recording time shown in the label below the bars. */
    void draw(TFT_eSPI& tft, int cx, int barCenterY, uint32_t phase, float seconds);

    /** Free the sprite.  Safe to call when no sprite is allocated (idempotent). */
    void release();

private:
    TFT_eSprite* spr_  = nullptr;
    int          sprW_ = 0;
    int          sprH_ = 0;
};

}  // namespace ui
