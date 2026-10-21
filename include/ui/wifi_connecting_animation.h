#pragma once
#include <stdint.h>

namespace ui {
// Reuse the existing dot, one-arc, two-arc and full-strength masks.
// A complete rise takes 960 ms, then starts again at the dot.
constexpr int8_t wifiConnectingTier(uint32_t elapsedMs) {
  return 1 + (elapsedMs / 240u) % 4u;
}
}
