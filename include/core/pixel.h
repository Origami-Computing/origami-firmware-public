#pragma once
/**
 * @file core/pixel.h
 * @brief RGB565 ↔ RGB888 conversion, brightness adjustment, and image filter primitives.
 *
 * All functions are pure (no global state).  applyFilter() combines filter
 * application and optional brightness scaling in a single MCU-sized pass so
 * callers don't need to manage the Bayer matrix or channel arithmetic
 * themselves.
 *
 * These primitives are shared across any app that processes camera frames or
 * renders colour effects: camera viewfinder, future image-preview apps, etc.
 */
#include <stdint.h>

namespace core {
namespace pixel {

// 16-level grey palette in RGB565, evenly spaced 0x0000 → 0xFFFF.
// Useful for ordered-dither rendering on colour panels.
extern const uint16_t kGreyPal16[16];

enum Filter {
    Normal = 0,
    BW,
    Dither,
    Invert,
    Poster,
    FilterCount,
};

// Unpack RGB565 → full 8-bit channels.
// Uses bit-replication (r5→r8 = r5<<3|r5>>2) to preserve peak white (0x1F→0xFF).
inline void rgb565_to_888(uint16_t p, int& r8, int& g8, int& b8) {
    uint8_t r5 = (uint8_t)((p >> 11) & 0x1F);
    uint8_t g6 = (uint8_t)((p >> 5)  & 0x3F);
    uint8_t b5 = (uint8_t)(p & 0x1F);
    r8 = (r5 << 3) | (r5 >> 2);
    g8 = (g6 << 2) | (g6 >> 4);
    b8 = (b5 << 3) | (b5 >> 2);
}

// Pack RGB888 → RGB565 with per-channel clamping to [0, 255].
inline uint16_t rgb888_to_565(int r8, int g8, int b8) {
    if (r8 < 0) r8 = 0; else if (r8 > 255) r8 = 255;
    if (g8 < 0) g8 = 0; else if (g8 > 255) g8 = 255;
    if (b8 < 0) b8 = 0; else if (b8 > 255) b8 = 255;
    return (uint16_t)(((r8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (b8 >> 3));
}

// Scale all RGB channels by pct (0..100). Clamps pct to 5 to avoid total black.
uint16_t adjustBrightness(uint16_t p, int pct);

// Apply filter and optional brightness to a w×h block of RGB565 pixels in-place.
// brightnessPct=100 leaves brightness unchanged.
void applyFilter(uint16_t* pixels, int w, int h, Filter f, int brightnessPct = 100);

}  // namespace pixel
}  // namespace core
