#pragma once
#include "core/morning_alarm.h"
#include "ui/chibi_eyes.h"
#include <math.h>

namespace ui { namespace morning_eyes {

struct Pose {
  chibi_eyes::Expression expression;
  uint8_t heightPercent;
  int8_t rotationDegrees;
};

// Integer smoothstep. The durations here are <= 300ms; 64-bit arithmetic also
// keeps this safe if a later sequence chooses a longer duration.
inline uint8_t ease(uint32_t elapsed, uint32_t duration, uint8_t from, uint8_t to) {
  if (!duration || elapsed >= duration) return to;
  const int64_t t = elapsed, d = duration;
  return from + (int64_t(to) - from) * t * t * (3 * d - 2 * t) / (d * d * d);
}

// Pure timeline: a phase change immediately replaces the previous sequence.
// Caller computes elapsed with unsigned millis subtraction (wrap-safe).
inline Pose sample(core::morning_alarm::Phase phase, uint32_t elapsed) {
  using E = chibi_eyes::Expression;
  using P = core::morning_alarm::Phase;
  if (phase == P::Ringing) {
    // Rock the entire squeezed pair around its center: level -> one side ->
    // level -> other side. Pre-rendered 2-degree steps retain smooth edges.
    const float wave = sinf((elapsed % 1100u) * (6.2831853f / 1100.f));
    return {E::ShutTight, 100, (int8_t)(2 * lroundf(7.f * wave))};
  }
  if (phase == P::Sleeping) {
    if (elapsed < 180) return {E::ShutTight, 100, 0};
    if (elapsed < 430) return {E::Bored, ease(elapsed - 180, 250, 100, 55), 0};
    return {E::Sleep, 100, 0};
  }
  if (phase == P::Greeting) {
    if (elapsed < 250) return {E::Wake, ease(elapsed, 250, 40, 100), 0};
    return {E::Happy, 100, 0};
  }
  return {E::Wake, 100, 0};
}

} } // namespace ui::morning_eyes
