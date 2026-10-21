#include "ui/mic_icon.h"

#include "config/ui_theme.h"

namespace ui {

static constexpr int kMicCapW   = 22;
static constexpr int kMicCapH   = 34;
static constexpr int kMicStandH = 12;
static constexpr int kMicStemH  = 4;
static constexpr int kMicBaseW  = 22;

int micIconHeight() {
    return kMicCapH + kMicStandH + kMicStemH + 2;
}

static void micInnerRect(int cx, int cy, int* outLeft, int* outTop, int* outW, int* outH) {
    const int capLeft = cx - kMicCapW / 2;
    const int capTop  = cy;
    *outLeft = capLeft + 2;
    *outTop  = capTop + 2;
    *outW    = kMicCapW - 4;
    *outH    = kMicCapH - 4;
}

static uint16_t levelColor(uint8_t lvl) {
    // 0..255 → black → red (narrow-duty square-wave chiptune aesthetic for the recording state)
    return (uint16_t)(((lvl & 0xF8u) << 8));
}

void drawMicIconFrame(TFT_eSPI& tft, int cx, int cy, bool active) {
    const int capLeft = cx - kMicCapW / 2;
    const int capTop  = cy;
    const int capR    = kMicCapW / 2;

    // Wipe bounding rect so any previous glyph is erased cleanly.
    tft.fillRect(capLeft - 2, capTop - 1, kMicCapW + 4, micIconHeight() + 2, kUiBg);

    const uint16_t outlineColor = active ? kUiFg : kUiMuted;

    // Double-stroke capsule.
    tft.drawRoundRect(capLeft, capTop, kMicCapW, kMicCapH, capR, outlineColor);
    tft.drawRoundRect(capLeft + 1, capTop + 1, kMicCapW - 2, kMicCapH - 2, capR - 1, outlineColor);

    // Stand: two parallel vertical pairs + U-curve bottom.
    const int standY = capTop + kMicCapH + 2;
    tft.drawFastVLine(cx - kMicCapW / 2 + 2, standY, kMicStandH - 4, outlineColor);
    tft.drawFastVLine(cx - kMicCapW / 2 + 3, standY, kMicStandH - 4, outlineColor);
    tft.drawFastVLine(cx + kMicCapW / 2 - 3, standY, kMicStandH - 4, outlineColor);
    tft.drawFastVLine(cx + kMicCapW / 2 - 4, standY, kMicStandH - 4, outlineColor);
    for (int x = cx - kMicCapW / 2 + 2; x <= cx + kMicCapW / 2 - 3; x++) {
        tft.drawPixel(x, standY + kMicStandH - 4, outlineColor);
        tft.drawPixel(x, standY + kMicStandH - 3, outlineColor);
    }

    // Stem + base.
    const int stemTop = standY + kMicStandH - 3;
    tft.drawFastVLine(cx,     stemTop, kMicStemH, outlineColor);
    tft.drawFastVLine(cx + 1, stemTop, kMicStemH, outlineColor);
    const int baseY = stemTop + kMicStemH;
    tft.drawFastHLine(cx - kMicBaseW / 2, baseY,     kMicBaseW, outlineColor);
    tft.drawFastHLine(cx - kMicBaseW / 2, baseY + 1, kMicBaseW, outlineColor);
}

void drawMicIconLevel(TFT_eSPI& tft, int cx, int cy, bool active, uint8_t level) {
    int innerLeft, innerTop, innerW, innerH;
    micInnerRect(cx, cy, &innerLeft, &innerTop, &innerW, &innerH);

    if (active) {
        tft.fillRect(innerLeft, innerTop, innerW, innerH, kUiBg);
        const int fillH = (innerH * level) / 255;
        if (fillH > 0) {
            tft.fillRect(innerLeft, innerTop + (innerH - fillH), innerW, fillH, levelColor(level));
        }
    } else {
        tft.fillRect(innerLeft, innerTop, innerW, innerH, kUiPanel);
    }
}

}  // namespace ui
