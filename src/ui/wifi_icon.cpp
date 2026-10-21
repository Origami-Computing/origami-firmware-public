#include "ui/wifi_icon.h"
#include "hal/hal_display.h"
#include "assets/wifi_icons.h"
#include "assets/battery_icon.h"
#include <TB_TFT_eSPI.h>

namespace ui {
static uint16_t blendWifiInk(uint16_t fg, uint16_t bg, int a, int denominator) {
  const int r=(((fg>>11)&31)*a+((bg>>11)&31)*(denominator-a)+denominator/2)/denominator;
  const int g=(((fg>>5)&63)*a+((bg>>5)&63)*(denominator-a)+denominator/2)/denominator;
  const int b=((fg&31)*a+(bg&31)*(denominator-a)+denominator/2)/denominator;
  return (r<<11)|(g<<5)|b;
}
void drawWifiIcon(TFT_eSPI& display, int16_t x, int16_t y, int8_t tier, bool connected,
                  uint16_t foreground, uint16_t background, bool transparent) {
  using namespace assets::wifi_icons;
  static_assert(kWidth==kWifiIconSize && kHeight==kWifiIconSize,"Wi-Fi masks must be generated at display size");
  const unsigned state=connected?(tier<1?1:tier>4?4:tier):0;
  const uint8_t* pixels=kStates[state];
  uint16_t palette[32];
  const uint16_t muted=blendWifiInk(foreground,background,3,10);
  for(int a=0;a<16;a++){
    palette[a]=blendWifiInk(foreground,background,a,15);
    palette[16+a]=blendWifiInk(muted,background,a,15);
  }
  // Masks already contain final-size coverage; never point-sample a larger mask.
  for(int dy=0;dy<kWifiIconSize;++dy) {
    for(int dx=0;dx<kWifiIconSize;++dx) {
      const uint8_t mask=pgm_read_byte(pixels+dy*kWidth+dx);
      if (transparent) {
        const int alpha=mask&15;
        if (!alpha) continue;
        const uint16_t bg=hal::display::canvas().readPixel(x+dx,y+dy);
        const uint16_t fg=mask&16 ? blendWifiInk(foreground,bg,3,10) : foreground;
        display.drawPixel(x+dx,y+dy,blendWifiInk(fg,bg,alpha,15));
      } else display.drawPixel(x+dx,y+dy,palette[mask]);
    }
  }
}
}
