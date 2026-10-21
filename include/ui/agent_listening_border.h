#pragma once

#include <math.h>
#include <stdint.h>
#include "config/display_safe_area.h"

namespace ui { namespace agent_listening {
constexpr int kMinThickness = 8;
constexpr int kMaxThickness = 26;
constexpr int kRadius = 39;
constexpr float kFullScaleLevel = 80.f;

inline float visualLevel(uint8_t raw) {
    // The shared recorder already applies its silence floor. Lift quiet speech
    // perceptually: raw 8 reaches ~14 px and raw 16 reaches ~16 px; ordinary
    // louder speech reaches the 26 px cap at raw 80 instead of requiring 255.
    return 255.f * sqrtf((raw < kFullScaleLevel ? raw : kFullScaleLevel)/kFullScaleLevel);
}

// Same 0..255 raw-mic envelope as the standalone recorder. Silence remains
// visible; fast attack and slower release avoid jitter without a timed pulse.
struct Envelope {
    float level = 0.f;
    uint32_t lastMs = 0;
    void reset(uint32_t now) { level = 0.f; lastMs = now; }
    int advance(uint8_t input, uint32_t now) {
        const uint32_t elapsed = now - lastMs;
        lastMs = now;
        const float target = visualLevel(input);
        const float tau = target > level ? 70.f : 200.f;
        level += (target-level) * (1.f-expf(-float(elapsed)/tau));
        return kMinThickness + int(lroundf((kMaxThickness-kMinThickness)*level/255.f));
    }
};

// Number of solid red pixels at each end of a scanline. Painting spans instead
// of nested outlines fills every outside pixel, including rounded corners.
inline int edgeSpan(int thickness, int y, int width, int height, bool right = false) {
    if (thickness <= 0) return 0;
    // Thickness is visible inside the case opening. Hidden panel pixels remain
    // red too, so the case cannot expose a white seam along any outer edge.
    const auto inner = safe_area::inset(safe_area::opening(display_safe_area::kProfile),thickness);
    if (y < inner.y || y >= inner.y+inner.h) return right ? width-width/2 : width/2;
    int x = inner.x;
    while (x < inner.x+inner.radius && !safe_area::contains(inner,x,y)) ++x;
    return right ? width-(inner.x+inner.w-(x-inner.x)) : x;
}
}} // namespace ui::agent_listening
