#pragma once
#include <stdint.h>

namespace ui {
// Non-owning drawing surface for Scene::addBitmap(..., bitsPerPixel=4).
// Caller supplies ceil(width*height/2) bytes and keeps them alive while the scene
// references them. RGB565 rectangles become packed, dark-on-light coverage.
class CoverageSurface {
public:
    CoverageSurface(uint8_t* pixels, int width, int height)
        : pixels_(pixels), width_(width), height_(height) {}
    void fillRect(int x, int y, int width, int height, uint16_t color) {
        const int grey = (((color >> 11) & 31) * 255 / 31 +
                          ((color >> 5) & 63) * 255 / 63 +
                          (color & 31) * 255 / 31) / 3;
        const uint8_t coverage = (255 - grey) * 15 / 255;
        const int left = x > 0 ? x : 0;
        const int top = y > 0 ? y : 0;
        const int right = x + width < width_ ? x + width : width_;
        const int bottom = y + height < height_ ? y + height : height_;
        for (int row = top; row < bottom; ++row) {
            for (int col = left; col < right; ++col) {
                const int index = row * width_ + col;
                uint8_t& value = pixels_[index / 2];
                value = (index & 1) ? ((value & 0xf0) | coverage)
                                    : ((value & 0x0f) | (coverage << 4));
            }
        }
    }
private:
    uint8_t* pixels_;
    int width_, height_;
};
}
