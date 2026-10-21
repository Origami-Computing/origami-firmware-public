#include "ui/alpha_bitmap.h"
#include "hal/hal_display.h"

namespace ui {
void drawAlphaBitmap(TFT_eSPI& display, int16_t x, int16_t y, int16_t width, int16_t height,
                     const uint8_t* pixels, uint16_t sourceWidth, uint16_t sourceHeight,
                     uint16_t foreground, uint16_t background, bool transparent) {
  if (!pixels || width <= 0 || height <= 0 || !sourceWidth || !sourceHeight) return;
  uint16_t palette[16];
  for (int a = 0; a < 16; ++a) {
    const int r = (((foreground >> 11) & 31) * a + ((background >> 11) & 31) * (15 - a) + 7) / 15;
    const int g = (((foreground >> 5) & 63) * a + ((background >> 5) & 63) * (15 - a) + 7) / 15;
    const int b = ((foreground & 31) * a + (background & 31) * (15 - a) + 7) / 15;
    palette[a] = (r << 11) | (g << 5) | b;
  }
  if (transparent) {
    const uint32_t stride = (sourceWidth + 1u) / 2u;
    for (int dy=0; dy<height; ++dy) for (int dx=0; dx<width; ++dx) {
      const unsigned sx=unsigned(dx)*sourceWidth/width;
      const uint8_t pair=pgm_read_byte(pixels+unsigned(dy)*sourceHeight/height*stride+sx/2);
      const int a=sx&1 ? pair&15 : pair>>4;
      if (!a) continue;
      const uint16_t bg=hal::display::canvas().readPixel(x+dx,y+dy);
      const int r=(((foreground>>11)&31)*a+((bg>>11)&31)*(15-a)+7)/15;
      const int g=(((foreground>>5)&63)*a+((bg>>5)&63)*(15-a)+7)/15;
      const int b=((foreground&31)*a+(bg&31)*(15-a)+7)/15;
      display.drawPixel(x+dx,y+dy,(r<<11)|(g<<5)|b);
    }
    return;
  }
  display.fillRect(x, y, width, height, background);
  const uint32_t stride = (sourceWidth + 1u) / 2u;
  for (int dy = 0; dy < height; ++dy) {
    const uint32_t row = uint32_t(dy) * sourceHeight / height * stride;
    int runStart = 0;
    uint8_t previous = 0;
    for (int dx = 0; dx <= width; ++dx) {
      uint8_t alpha = 0;
      if (dx < width) {
        const uint32_t sx = uint32_t(dx) * sourceWidth / width;
        const uint8_t pair = pgm_read_byte(pixels + row + sx / 2);
        alpha = sx & 1 ? pair & 15 : pair >> 4;
      }
      if (alpha != previous) {
        if (previous) display.drawFastHLine(x + runStart, y + dy, dx - runStart, palette[previous]);
        previous = alpha;
        runStart = dx;
      }
    }
  }
}
}
