#pragma once

#include <TB_TFT_eSPI.h>

namespace ui {
namespace dialogue {

void drawFrame(TFT_eSprite& spr, int w, int h);

void drawScrollHint(TFT_eSprite& spr, int x, int y);

void drawMiniEyes(TFT_eSprite& spr, bool open);

void drawFrameDirect(TFT_eSPI& tft, int x, int y, int w, int h);

}  // namespace dialogue
}  // namespace ui
