#pragma once
#include <stdint.h>
#include <math.h>

namespace ui {
// sRGB relative luminance and contrast: https://www.w3.org/TR/WCAG22/#dfn-relative-luminance
inline float statusBarLuminance(uint16_t color) {
    auto linear = [](float c) { return c <= .04045f ? c / 12.92f : powf((c + .055f) / 1.055f, 2.4f); };
    return .2126f * linear(((color >> 11) & 31) / 31.f) +
           .7152f * linear(((color >> 5) & 63) / 63.f) +
           .0722f * linear((color & 31) / 31.f);
}

inline uint16_t statusBarInk(uint16_t background) {
    const float luminance = statusBarLuminance(background);
    // Choose the stronger of black/white, not an RGB average (which misreads blue).
    return (luminance + .05f) / .05f >= 1.05f / (luminance + .05f) ? 0x0000 : 0xFFFF;
}

inline uint16_t statusBarMutedInk(uint16_t background) {
    const uint16_t ink = statusBarInk(background);
    const int r = (((background >> 11) & 31) + 3 * ((ink >> 11) & 31)) / 4;
    const int g = (((background >> 5) & 63) + 3 * ((ink >> 5) & 63)) / 4;
    const int b = ((background & 31) + 3 * (ink & 31)) / 4;
    const uint16_t muted = (r << 11) | (g << 5) | b;
    const float a = statusBarLuminance(background), z = statusBarLuminance(muted);
    // Even inactive/disconnected arcs stay at least 3:1 against the scene.
    const float contrast = a > z ? (a + .05f) / (z + .05f) : (z + .05f) / (a + .05f);
    return contrast >= 3.f ? muted : ink;
}
}  // namespace ui
