#include "ui/recording_meter.h"
#include "ui/safe_area_draw.h"

#include <Arduino.h>
#include <math.h>

namespace ui {

static constexpr uint16_t kBarColor  = 0xF800;  // red
static constexpr uint16_t kBarShade  = 0x8000;  // dark red edge highlight
static constexpr uint16_t kTimerFg   = 0x4208;  // muted gray
static constexpr uint16_t kBg        = 0xFFFF;  // white

void RecordingMeter::draw(TFT_eSPI& tft, int cx, int barCenterY, uint32_t phase, float seconds) {
    static constexpr int N    = kBarCount;
    static constexpr int BW   = kBarW;
    static constexpr int GAP  = kBarGap;
    static constexpr int BH   = kBandH;
    static constexpr int timerPad = 14;

    const int sprW   = N * (BW + GAP) + 12;
    const int sprH   = BH + timerPad;
    if (safe_area::applies(tft)) {
        const auto box = safe_area::fitBox({cx-sprW/2,barCenterY-BH/2,sprW,sprH});
        cx = box.x+sprW/2;
        barCenterY = box.y+BH/2;
    }
    const int startX = cx - (N * (BW + GAP) - GAP) / 2;
    const int scrX   = startX - 6;
    const int bandTop = barCenterY - BH / 2;
    const int barYC   = barCenterY - bandTop;

    auto drawDirect = [&]() {
        tft.fillRect(scrX, bandTop, sprW, BH, kBg);
        for (int i = 0; i < N; i++) {
            float ph = (float)phase * 0.08f + (float)i * 0.56f;
            int   h  = 12 + (int)(38.0f * fabsf(sinf(ph)));
            int   bx = startX + i * (BW + GAP);
            tft.fillRect(bx, barCenterY - h / 2, BW, h, kBarColor);
            tft.fillRect(bx, barCenterY - h / 2, BW, 2, kBarShade);
            tft.fillRect(bx, barCenterY + h / 2 - 2, BW, 2, kBarShade);
        }
        char buf[12];
        snprintf(buf, sizeof(buf), "%.1f s", seconds);
        tft.fillRect(cx - 28, barCenterY + BH / 2 - 8, 56, 11, kBg);
        tft.setTextColor(kTimerFg, kBg);
        tft.setTextSize(1);
        int px = cx - (int)strlen(buf) * 3;
        tft.setCursor(px < 0 ? 0 : px, barCenterY + BH / 2 - 8);
        tft.print(buf);
    };

    if (!spr_ || !spr_->created() || sprW_ != sprW || sprH_ != sprH) {
        release();
        spr_ = new TFT_eSprite(&tft);
        spr_->setColorDepth(16);
        if (!spr_->createSprite(sprW, sprH)) {
            delete spr_;
            spr_ = nullptr;
            sprW_ = sprH_ = 0;
            drawDirect();
            return;
        }
        sprW_ = sprW;
        sprH_ = sprH;
    }

    spr_->fillSprite(kBg);
    for (int i = 0; i < N; i++) {
        float ph = (float)phase * 0.08f + (float)i * 0.56f;
        int   h  = 12 + (int)(38.0f * fabsf(sinf(ph)));
        int   bx = startX + i * (BW + GAP);
        int   lx = bx - scrX;
        int   y0 = barYC - h / 2;
        if (y0 < 0) y0 = 0;
        if (h > 0 && lx >= 0 && lx + BW <= sprW) {
            spr_->fillRect(lx, y0, BW, h, kBarColor);
            spr_->fillRect(lx, y0, BW, 2, kBarShade);
            if (h > 2) spr_->fillRect(lx, y0 + h - 2, BW, 2, kBarShade);
        }
    }

    char buf[12];
    snprintf(buf, sizeof(buf), "%.1f s", seconds);
    spr_->setTextDatum(MC_DATUM);
    spr_->setTextFont(1);
    spr_->setTextSize(1);
    spr_->setTextColor(kTimerFg, kBg);
    spr_->drawString(buf, sprW / 2, BH + 7);
    spr_->setTextDatum(TL_DATUM);

    spr_->pushSprite(scrX, bandTop);
}

void RecordingMeter::release() {
    if (spr_) {
        if (spr_->created()) spr_->deleteSprite();
        delete spr_;
        spr_ = nullptr;
    }
    sprW_ = sprH_ = 0;
}

}  // namespace ui
