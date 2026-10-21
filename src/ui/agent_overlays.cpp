#include "ui/agent_overlays.h"
#include "ui/safe_area_draw.h"

#include <Arduino.h>
#include <cstring>

#include "config/ui_layout.h"
#include "config/agent_layout.h"
#include "config/ui_theme.h"
#include "hal/hal_display.h"
#include "ui/home_eyes.h"
#include "ui/kami_dialogue.h"
#include "ui/pixel_dialogue_frame.h"
#include "ui/pixel_font.h"
#include "ui/text_wrap.h"

namespace ui {

static constexpr uint16_t kDim = 0x4208;  // muted gray hint text

static void centered(TFT_eSPI& tft, const char* s, int y, uint16_t fg, uint16_t bg, uint8_t sz = 1) {
    tft.setTextFont(1);
    tft.setTextSize(sz);
    tft.setTextColor(fg, bg);
    int px = (tft.width() - tft.textWidth(s,1)) / 2;
    tft.setTextDatum(TL_DATUM);
    safe_area::drawString(tft,s,px < 0 ? 0 : px,y);
}

static void centeredAt(TFT_eSPI& tft, const char* s, int centerX, int y,
                       uint16_t fg, uint16_t bg, uint8_t sz = 1) {
    tft.setTextFont(1);
    tft.setTextSize(sz);
    tft.setTextColor(fg, bg);
    const int px = centerX - (tft.textWidth(s,1)) / 2;
    tft.setTextDatum(TL_DATUM);
    safe_area::drawString(tft,s,px < 0 ? 0 : px,y);
}

void paintPttPrepare(TFT_eSPI& tft, int topY) {
    hal::display::clearContentArea(kUiBg);
    (void)topY;
    centeredAt(tft, "Preparing mic", agent_layout::kPrepareTitleCenterX,
               agent_layout::kPrepareTitleY, kUiFg, kUiBg, 2);
    centeredAt(tft, "Please wait", agent_layout::kPrepareHintCenterX,
               agent_layout::kPrepareHintY, kDim, kUiBg, 1);
}

void paintRecordingStatic(TFT_eSPI& tft, int topY) {
    hal::display::clearContentArea(kUiBg);
    (void)topY;
    centeredAt(tft, "Recording", agent_layout::kRecordingTitleCenterX,
               agent_layout::kRecordingTitleY, kUiFg, kUiBg, 2);
    centeredAt(tft, "Release A or B to send", agent_layout::kRecordingHintCenterX,
               agent_layout::kRecordingHintY, kDim, kUiBg, 1);
}

void paintThinkingScreen(TFT_eSPI& tft, int topY) {
    hal::display::releaseSpiForSd();

    if (!ui::home_eyes::isLoaded()) {
        ui::home_eyes::beginSessionSingleFrame(tft);
    }

    hal::display::clearContentArea(kUiBg);

    if (ui::home_eyes::isProcedural()) {
        hal::display::fixColorInversion();
        tft.fillRect(0, topY, tft.width(), tft.height() - topY, TFT_WHITE);
        ui::home_eyes::resetProceduralPaintState(0);
    }

    const uint32_t now = millis();
    (void)ui::home_eyes::advanceOpenEyeOverlay(tft, 0, now);
    ui::home_eyes::fillBmpBottomLetterboxAfterAdvance(tft);

    static const char kThinking[] = "Thinking...";
    using ui::text::kPokeCW;
    tft.setTextFont(1); tft.setTextSize(2);
    const int thinkW = tft.textWidth(kThinking);
    const int thinkX = agent_layout::kThinkingCenterX - thinkW / 2;
    ui::text::wrappedPokemon(tft, kThinking, thinkX < 0 ? 0 : thinkX,
                             agent_layout::kThinkingY, tft.width(), 1, kUiFg, kUiBg);
}

void paintKamiNotice(TFT_eSPI& tft,apps::agent::notice::Kind kind,int topY){
    using namespace apps::agent::notice;
    hal::display::clearContentArea(kUiBg);
    ui::kami_dialogue::drawAvatar(tft,26,topY+2,true,"neutral");
    ui::pixel_dialogue::draw(tft,{22,78,236,136},kUiFg,kUiBg);
    ui::text::wrappedSized(tft,message(kind),36,90,208,5,kUiFg,kUiBg,2);
    const char* hint=needsWifi(kind)?"A Back   B Wi-Fi":kind==Kind::Auth?"A Back   B Settings":"A Back   A+B Talk";
    centered(tft,hint,224,kUiFg,kUiBg,1);
    hal::display::markDirty(topY,tft.height()-topY);
}
void paintKamiError(TFT_eSPI& tft,const char* err,int topY){
    paintKamiNotice(tft,apps::agent::notice::classify(err,true),topY);
}

}  // namespace ui
