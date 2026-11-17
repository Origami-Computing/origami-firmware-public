#pragma once
#include <stdint.h>
namespace core {
namespace cine {
// Short to long. Positive service steps lengthen integration; the UI inverts wheel motion.
constexpr uint32_t shutterSpeedsUs[] = {1000,  1250,  1563,  2000,  2500,  3125,  4000,
                                        5000,  6250,  8000,  10000, 12500, 16667, 20000,
                                        25000, 33333, 40000, 50000, 62500, 80000, 100000};
constexpr unsigned shutterCount = sizeof(shutterSpeedsUs) / sizeof(shutterSpeedsUs[0]);
inline unsigned nearestShutter(uint32_t us) {
  unsigned best = 0;
  uint32_t distance = ~uint32_t(0);
  for (unsigned i = 0; i < shutterCount; ++i) {
    uint32_t d = us > shutterSpeedsUs[i] ? us - shutterSpeedsUs[i] : shutterSpeedsUs[i] - us;
    if (d < distance) {
      distance = d;
      best = i;
    }
  }
  return best;
}
// Sixteen integration ticks per sensor row. Floor the 100ms bound so physical
// integration cannot exceed it through quantization. Retain four frame-margin rows.
inline uint32_t shutterTicks(uint32_t us, uint16_t hts, uint16_t vts, uint32_t clock) {
  if (!hts || vts <= 4 || !clock)
    return 0;
  if (us < 1000)
    us = 1000;
  if (us > 100000)
    us = 100000;
  uint64_t denominator = uint64_t(hts) * 1000000;
  uint64_t ticks = (uint64_t(us) * clock * 16 + denominator / 2) / denominator;
  uint64_t cap = uint64_t(100000) * clock * 16 / denominator, frame = uint64_t(vts - 4) * 16;
  if (cap > frame)
    cap = frame;
  if (ticks > cap)
    ticks = cap;
  return ticks < 16 ? 16 : uint32_t(ticks);
}
inline uint32_t shutterMicros(uint32_t ticks, uint16_t hts, uint32_t clock) {
  return clock ? uint32_t((uint64_t(ticks) * hts * 1000000 + uint64_t(clock) * 8) /
                          (uint64_t(clock) * 16))
               : 0;
}
} // namespace cine
} // namespace core
