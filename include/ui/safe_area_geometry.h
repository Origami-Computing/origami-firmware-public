#pragma once

// Pure geometry shared by the calibration renderer and compile-time tests.
// Insets count hidden pixels; right/bottom coordinates are exclusive.
namespace ui { namespace safe_area {
constexpr int kWidth = 280;
constexpr int kHeight = 240;
struct Config { int left, top, right, bottom, radius, padding; };
struct Rect { int x, y, w, h, radius; };
constexpr Config defaults() { return {0, 0, 0, 0, 0, 4}; }
constexpr int min(int a, int b) { return a < b ? a : b; }
constexpr int max(int a, int b) { return a > b ? a : b; }
constexpr Rect opening(const Config& c) {
    return {c.left, c.top, kWidth-c.left-c.right, kHeight-c.top-c.bottom, c.radius};
}
constexpr Rect inset(const Rect& r, int n) {
    return {r.x+n, r.y+n, r.w-2*n, r.h-2*n, max(0, r.radius-n)};
}
constexpr bool valid(const Config& c) {
    return c.left >= 0 && c.left <= kWidth && c.right >= 0 && c.right <= kWidth &&
           c.top >= 0 && c.top <= kHeight && c.bottom >= 0 && c.bottom <= kHeight &&
           opening(c).w >= 64 && opening(c).h >= 64 &&
           c.radius >= 0 && c.radius <= min(opening(c).w, opening(c).h)/2 &&
           c.padding >= 0 && c.padding <= 24 &&
           2*c.padding < min(opening(c).w, opening(c).h);
}
constexpr int square(int x) { return x*x; }
constexpr int clamp(int x, int lo, int hi) { return min(max(x, lo), hi); }
// Pixel centers are doubled to retain half-pixel precision with integer math.
constexpr bool contains(const Rect& r, int x, int y) {
    return r.w > 0 && r.h > 0 && x >= r.x && x < r.x+r.w && y >= r.y && y < r.y+r.h &&
        square(2*x+1-clamp(2*x+1, 2*(r.x+r.radius), 2*(r.x+r.w-r.radius))) +
        square(2*y+1-clamp(2*y+1, 2*(r.y+r.radius), 2*(r.y+r.h-r.radius))) <= square(2*r.radius);
}
}} // namespace ui::safe_area
