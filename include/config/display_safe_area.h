#pragma once
#include "ui/safe_area_geometry.h"

namespace display_safe_area {
// Measured in Screen Fit on the cased rev-4 device, 2026-09-15.
// Keep the 280x240 physical canvas. These are layout constraints, not a resize.
constexpr ui::safe_area::Config kProfile{3, 5, 1, 1, 39, 2};
static_assert(ui::safe_area::valid(kProfile), "Invalid measured display opening");
constexpr int kStatusBarHeight = 38; // fits 28px Wi-Fi glyph below the top lip
}
