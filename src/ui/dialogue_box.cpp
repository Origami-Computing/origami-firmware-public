#include "ui/dialogue_box.h"
#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "ui/pixel_dialogue_frame.h"

namespace ui {
namespace dialogue {

void drawFrame(TFT_eSprite& spr, int w, int h) {
  pixel_dialogue::draw(spr,{0,0,w,h},kUiDialogueBorder,kUiDialogueFill);
}

void drawScrollHint(TFT_eSprite& spr, int x, int y) {
  spr.fillTriangle(x, y, x + 6, y, x + 3, y + 3, kUiDialogueHint);
}

void drawMiniEyes(TFT_eSprite& spr, bool open) {
  spr.fillSprite(kUiBg);

  const int eyeW = 14;
  const int eyeH = open ? 18 : 4;
  const int gap  = 6;
  const int totalW = eyeW * 2 + gap;
  const int sx = (spr.width() - totalW) / 2;
  const int cy = spr.height() / 2;

  if (open) {
    int topY = cy - eyeH / 2;
    spr.fillRoundRect(sx, topY, eyeW, eyeH, 4, kUiFg);
    spr.fillRoundRect(sx + eyeW + gap, topY, eyeW, eyeH, 4, kUiFg);
  } else {
    int topY = cy - eyeH / 2;
    spr.fillRoundRect(sx, topY, eyeW, eyeH, 2, kUiFg);
    spr.fillRoundRect(sx + eyeW + gap, topY, eyeW, eyeH, 2, kUiFg);
  }
}

void drawFrameDirect(TFT_eSPI& tft, int x, int y, int w, int h) {
  pixel_dialogue::draw(tft,{x,y,w,h},kUiDialogueBorder,kUiDialogueFill);
}

}  // namespace dialogue
}  // namespace ui
