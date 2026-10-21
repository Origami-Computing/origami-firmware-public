#include "ui/color.h"

namespace ui {

uint16_t hsv565(uint16_t hueDeg, uint8_t sat, uint8_t val) {
    hueDeg %= 360;
    const uint8_t region = (uint8_t)(hueDeg / 60);
    const uint8_t f      = (uint8_t)((hueDeg % 60) * 255 / 60);  // 0..255 across the sextant

    const uint8_t p = (uint8_t)((uint16_t)val * (255 - sat) / 255);
    const uint8_t q = (uint8_t)((uint16_t)val * (255 - (uint16_t)sat * f / 255) / 255);
    const uint8_t t = (uint8_t)((uint16_t)val * (255 - (uint16_t)sat * (255 - f) / 255) / 255);

    uint8_t r, g, b;
    switch (region) {
        case 0:  r = val; g = t;   b = p;   break;
        case 1:  r = q;   g = val; b = p;   break;
        case 2:  r = p;   g = val; b = t;   break;
        case 3:  r = p;   g = q;   b = val; break;
        case 4:  r = t;   g = p;   b = val; break;
        default: r = val; g = p;   b = q;   break;  // region 5
    }
    return rgb565(r, g, b);
}

}  // namespace ui
