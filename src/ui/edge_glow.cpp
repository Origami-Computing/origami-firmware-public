#include "ui/edge_glow.h"

#include <TB_TFT_eSPI.h>
#include <math.h>

#include "hal/hal_display.h"
#include "ui/color.h"
#include "config/display_safe_area.h"

namespace ui {
namespace {

// ─── Look & feel ────────────────────────────────────────────────────────────
// Tunable on-device. Corner radius matches the rounded glass: the 1.69" 240x280
// panel is ~0.116 mm/px, so 4.6–5 mm ≈ 40–43 px. The ring is a uniform-width
// rounded border on all four sides, full-saturation/full-brightness (vivid). The
// mic level drives the *thickness* (a volume bar), not the colour.

constexpr int      kMinThick  = 11;      // band depth when quiet
constexpr int      kMaxThick  = 17;      // band depth at full volume
constexpr uint16_t kHueSpan   = 360;     // degrees of hue around the screen
constexpr uint32_t kSweepMs   = 3200;    // time for the hue wheel to rotate once
constexpr uint8_t  kSat       = 255;     // saturation (vivid)
constexpr uint8_t  kVal       = 255;     // brightness (vivid — never dimmed)

}  // namespace

void paintEdgeGlow(TFT_eSPI& tft, uint32_t nowMs, uint8_t intensity) {
    const int W = tft.width();
    const int H = tft.height();
    const auto opening = safe_area::opening(display_safe_area::kProfile);
    float r = (float)opening.radius;
    if (r * 2.f > (float)W) r = (float)W / 2.f;
    if (r * 2.f > (float)H) r = (float)H / 2.f;

    const int thick = kMinThick + (kMaxThick - kMinThick) * (int)intensity / 255;

    const float cx = opening.x + (float)opening.w * 0.5f;
    const float cy = opening.y + (float)opening.h * 0.5f;
    const float hx = (float)opening.w * 0.5f - r;
    const float hy = (float)opening.h * 0.5f - r;

    const float phase = (float)(nowMs % kSweepMs) * 360.f / (float)kSweepMs;

    for (int y = opening.y; y < opening.y+opening.h; ++y) {
        const float qy = fabsf((float)y - cy) - hy;
        for (int x = opening.x; x < opening.x+opening.w; ++x) {
            if (!safe_area::contains(opening,x,y)) continue;
            // Signed distance to the rounded-rect boundary (negative = inside).
            const float qx = fabsf((float)x - cx) - hx;
            float sd;
            if (qx <= 0.f && qy <= 0.f) {
                sd = (qx > qy ? qx : qy) - r;          // interior — no sqrt
            } else {
                const float ox = qx > 0.f ? qx : 0.f;
                const float oy = qy > 0.f ? qy : 0.f;
                sd = sqrtf(ox * ox + oy * oy) - r;     // near/outside a corner
            }
            // Uniform-width band just inside the boundary. sd >= 0 (beyond the
            // rounded edge) also fills, so the square corners leave no white gap.
            if (sd < (float)(-thick)) continue;

            // Hue by angle around the centre → a smooth wheel that the phase spins.
            const float ang = atan2f((float)y - cy, (float)x - cx);   // [-pi, pi]
            const float t = (ang + 3.14159265f) / 6.28318531f;        // [0, 1)
            const uint16_t hue = (uint16_t)fmodf(t * (float)kHueSpan + phase, 360.f);
            tft.drawPixel(x, y, hsv565(hue, kSat, kVal));
        }
    }

    // The ring spans every band, so mark the whole canvas dirty (see flush()).
    hal::display::markDirty(0, H);
}

}  // namespace ui
