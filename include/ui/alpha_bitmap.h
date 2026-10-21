#pragma once
#include <stdint.h>
class TFT_eSPI;

namespace ui {
// Draw row-major high-nibble-first 4-bit coverage against the supplied background.
// Source rows are padded to whole bytes. Destination scaling uses nearest sampling.
void drawAlphaBitmap(TFT_eSPI& display, int16_t x, int16_t y, int16_t width, int16_t height,
                     const uint8_t* pixels, uint16_t sourceWidth, uint16_t sourceHeight,
                     uint16_t foreground, uint16_t background, bool transparent = false);
}
