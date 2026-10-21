#pragma once

#include <stdint.h>

struct Tween {
  int32_t from, to;
  uint32_t startMs, durationMs;

  int32_t value(uint32_t now) const {
    if (now <= startMs) return from;
    uint32_t elapsed = now - startMs;
    if (elapsed >= durationMs) return to;
    int32_t t256 = (int32_t)(elapsed * 256 / durationMs);
    int32_t ease = t256 * (512 - t256) / 256;
    return from + (to - from) * ease / 256;
  }

  bool done(uint32_t now) const {
    return (now - startMs) >= durationMs;
  }
};
