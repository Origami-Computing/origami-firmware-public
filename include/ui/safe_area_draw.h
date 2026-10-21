#pragma once
#include <TB_TFT_eSPI.h>
#include "ui/safe_area_layout.h"

namespace ui { namespace safe_area {
// Only full display surfaces use physical bounds. Local sprites retain their
// own coordinate system (meter sprites, bitmap caches, and animation layers).
bool applies(TFT_eSPI& surface);
int16_t drawString(TFT_eSPI& surface, const char* text, int x, int y,
                   uint8_t font = 0, Region region = Region::Content);
int16_t drawCentreString(TFT_eSPI& surface, const char* text, int x, int y, uint8_t font);
int16_t drawRightString(TFT_eSPI& surface, const char* text, int x, int y, uint8_t font);
// Scene labels also use their declared width, so a long caption cannot cover
// a neighboring row indicator. Truncation preserves the active font size.
int16_t drawLabel(TFT_eSPI& surface, const char* text, Box box, uint8_t datum,
                 uint8_t font = 0, Region region = Region::Content);
}}
