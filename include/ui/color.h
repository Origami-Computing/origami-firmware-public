#pragma once

#include <stdint.h>

namespace ui {

/** Pack 8-bit RGB into a RGB565 word. */
constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/**
 * HSV → RGB565. hueDeg wraps mod 360; sat and val are 0..255.
 * Integer-only (no float), safe to call per-pixel on the MCU.
 */
uint16_t hsv565(uint16_t hueDeg, uint8_t sat, uint8_t val);

}  // namespace ui
