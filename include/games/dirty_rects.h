#pragma once
/**
 * Dirty rectangles for the games that repaint only what changed (bowling_view, tennis_view): half-open rectangles
 * and a merge that keeps their number small. Pure, header-only.
 */
#include <stdint.h>
#include <string.h>

namespace games {
namespace dirty {

struct Rect {
  int16_t x0, y0, x1, y1;  // half-open
};

inline int area(const Rect& r) { return (r.x1 - r.x0) * (r.y1 - r.y0); }
inline bool empty(const Rect& r) { return r.x1 <= r.x0 || r.y1 <= r.y0; }
inline Rect unite(const Rect& a, const Rect& b) {
  return Rect{a.x0 < b.x0 ? a.x0 : b.x0, a.y0 < b.y0 ? a.y0 : b.y0, a.x1 > b.x1 ? a.x1 : b.x1, a.y1 > b.y1 ? a.y1 : b.y1};
}
inline bool overlap(const Rect& a, const Rect& b) { return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1; }
inline bool same(const Rect& a, const Rect& b) { return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1; }

// Merge rectangles that overlap (or nearly: merging costs less than the seam), then down to `cap`. Returns the count.
inline int mergeRects(Rect* r, int n, int cap) {
  for (bool again = true; again;) {
    again = false;
    for (int i = 0; i < n && !again; ++i)
      for (int j = i + 1; j < n && !again; ++j) {
        const Rect u = unite(r[i], r[j]);
        if (overlap(r[i], r[j]) || area(u) <= area(r[i]) + area(r[j]) + 256) {
          r[i] = u;
          r[j] = r[--n];
          again = true;
        }
      }
  }
  while (n > cap) {
    int bi = 0, bj = 1, best = 1 << 30;
    for (int i = 0; i < n; ++i)
      for (int j = i + 1; j < n; ++j) {
        const int grow = area(unite(r[i], r[j])) - area(r[i]) - area(r[j]);
        if (grow < best) { best = grow; bi = i; bj = j; }
      }
    r[bi] = unite(r[bi], r[bj]);
    r[bj] = r[--n];
  }
  return n;
}

// FNV-1a over a primitive's parameters: an item whose hash and box are unchanged is not repainted.
struct Hash {
  uint32_t v;
  explicit Hash(uint32_t tag) : v(2166136261u ^ tag) {}
  Hash& u(uint32_t x) {
    for (int i = 0; i < 4; ++i) { v ^= (x >> (8 * i)) & 0xFF; v *= 16777619u; }
    return *this;
  }
  Hash& f(float x) {
    uint32_t b;
    memcpy(&b, &x, 4);
    return u(b);
  }
  Hash& p(const void* q) { return u((uint32_t)(uintptr_t)q); }
  Hash& s(const char* t) {
    while (t && *t) u((uint8_t)*t++);
    return u(0);
  }
};

}  // namespace dirty
}  // namespace games
