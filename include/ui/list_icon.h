#pragma once

namespace ui {
// Native row icon geometry. Assets are 20x20, row-major 4-bit coverage masks.
// Keep an 8px gap to the existing 12px-wide menu font; do not scale at runtime.
constexpr int kListIconSize = 20;
constexpr int kListIconTextOffset = kListIconSize + 8;
}
