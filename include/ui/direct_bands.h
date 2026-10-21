#pragma once
#include "hal/hal_display.h"

namespace ui {
// Shared UI-task scratch: 280x32 RGB565, allocated lazily and retained for reuse.
// Synchronous, non-reentrant paints must cover the requested region and must not
// retain the surface. Coordinates stay in canvas space; only dirty pixels cross
// SPI. Caller supplies an in-canvas region (width <= 280).
namespace detail { TFT_eSprite* directBandSprite(); }
struct BandSurface {
    TFT_eSprite& sprite;
    int x, y;
    void fillRect(int px, int py, int w, int h, uint16_t color) {
        sprite.fillRect(px - x, py - y, w, h, color);
    }
};
template <typename Paint>
void drawDirectBands(int x, int y, int w, int h, Paint paint) {
    auto* storage = detail::directBandSprite();
    if (!storage) return;
    auto& band = *storage;
    for (int offset = 0; offset < h; offset += 32) {
        const int rows = h - offset < 32 ? h - offset : 32;
        band.setViewport(0, 0, w, rows, false);
        BandSurface surface{band, x, y + offset};
        paint(surface);
        band.resetViewport();
        hal::display::beginDirectDraw();
        band.pushSprite(x, y + offset, 0, 0, w, rows);
        hal::display::endDirectDraw();
    }
}
}
