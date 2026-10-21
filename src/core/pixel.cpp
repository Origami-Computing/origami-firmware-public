/**
 * @file core/pixel.cpp
 * @brief RGB565 image filter implementations — BW luma, 1-bit Bayer dither, invert, poster.
 *
 * Bayer matrix: 4×4 ordered dither, values 0..15 scaled to threshold range 8..248
 * so no pixel is ever forced fully on or fully off by the matrix alone.
 * Rec. 601 luma coefficients (77, 150, 29) are used for all greyscale conversions.
 */
#include "core/pixel.h"

namespace core {
namespace pixel {

const uint16_t kGreyPal16[16] = {
    0x0000, 0x1082, 0x2104, 0x3186, 0x4228, 0x528A, 0x630C, 0x738E,
    0x8410, 0x9492, 0xA514, 0xB596, 0xC618, 0xD69A, 0xE71C, 0xFFFF,
};

uint16_t adjustBrightness(uint16_t p, int pct) {
    if (pct >= 100) return p;
    if (pct <= 5) pct = 5;
    int r8, g8, b8;
    rgb565_to_888(p, r8, g8, b8);
    r8 = (r8 * pct) / 100;
    g8 = (g8 * pct) / 100;
    b8 = (b8 * pct) / 100;
    return rgb888_to_565(r8, g8, b8);
}

void applyFilter(uint16_t* p, int w, int h, Filter filter, int brightnessPct) {
    static const uint8_t B[4][4] = {
        { 0,  8,  2, 10},
        {12,  4, 14,  6},
        { 3, 11,  1,  9},
        {15,  7, 13,  5},
    };

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint16_t p0 = p[y * w + x];

            switch (filter) {
                case BW: {
                    int r8, g8, b8;
                    rgb565_to_888(p0, r8, g8, b8);
                    int y8 = (r8 * 77 + g8 * 150 + b8 * 29) >> 8;
                    p0 = rgb888_to_565(y8, y8, y8);
                    break;
                }
                case Dither: {
                    int r8, g8, b8;
                    rgb565_to_888(p0, r8, g8, b8);
                    int y8  = (r8 * 77 + g8 * 150 + b8 * 29) >> 8;
                    int thr = B[y & 3][x & 3] * 16 + 8;
                    p0 = (y8 > thr) ? 0xFFFF : 0x0000;
                    break;
                }
                case Invert: {
                    // Channel-wise invert directly in 565 — no unpack needed.
                    uint8_t r5 = (uint8_t)((p0 >> 11) & 0x1F);
                    uint8_t g6 = (uint8_t)((p0 >> 5)  & 0x3F);
                    uint8_t b5 = (uint8_t)(p0 & 0x1F);
                    p0 = (uint16_t)(((uint8_t)(31 - r5) << 11) |
                                    ((uint8_t)(63 - g6) << 5)  |
                                     (uint8_t)(31 - b5));
                    break;
                }
                case Poster: {
                    int r8, g8, b8;
                    rgb565_to_888(p0, r8, g8, b8);
                    int n = B[y & 3][x & 3] - 8;  // -8..+7 dither nudge softens abrupt steps
                    r8 = (((r8 + n) >> 6) << 6) + 32;
                    g8 = (((g8 + n) >> 6) << 6) + 32;
                    b8 = (((b8 + n) >> 6) << 6) + 32;
                    p0 = rgb888_to_565(r8, g8, b8);
                    break;
                }
                case Normal:
                default:
                    break;
            }

            p[y * w + x] = adjustBrightness(p0, brightnessPct);
        }
    }
}

}  // namespace pixel
}  // namespace core
