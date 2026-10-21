#pragma once
#include "config/display_safe_area.h"
#include <climits>

namespace ui { namespace safe_area {
struct Box { int x, y, w, h; };
struct Span { int left, right; }; // right is exclusive
// Content excludes system chrome; Screen includes its reserved band.
enum class Region { Content, Screen };
constexpr Rect contentBoundary() {
    return inset(opening(display_safe_area::kProfile), display_safe_area::kProfile.padding);
}
constexpr int contentTop() { return display_safe_area::kStatusBarHeight; }

namespace detail {
constexpr int firstPixel(Rect r, int y, int x) {
    return x >= r.x+r.radius || contains(r,x,y) ? x : firstPixel(r,y,x+1);
}
constexpr Span symmetricSpan(Rect r, int left) { return {left,r.x+r.w-(left-r.x)}; }
constexpr Span intersect(Span a, Span b) { return {max(a.left,b.left),min(a.right,b.right)}; }
constexpr Box fitHeight(Box b, int top, int bottom) {
    return {b.x,b.y,b.w,clamp(b.h,1,bottom-top)};
}
constexpr Box fitY(Box b, int top, int bottom) {
    return {b.x,clamp(b.y,top,bottom-b.h),b.w,b.h};
}
constexpr Box fitWidth(Box b, Span span) {
    return {b.x,b.y,clamp(b.w,1,span.right-span.left),b.h};
}
constexpr Box fitX(Box b, Span span) {
    return {clamp(b.x,span.left,span.right-b.w),b.y,b.w,b.h};
}
}
constexpr Span rowSpan(int y) {
    return y < contentBoundary().y || y >= contentBoundary().y+contentBoundary().h
        ? Span{contentBoundary().x,contentBoundary().x}
        : detail::symmetricSpan(contentBoundary(),detail::firstPixel(contentBoundary(),y,contentBoundary().x));
}
// Check BOTH end rows: convexity guarantees that the intervening rows fit too.
constexpr Span bandSpan(int y, int height) {
    return detail::intersect(rowSpan(y),rowSpan(y+max(1,height)-1));
}
namespace detail {
constexpr Box fitHorizontal(Box b) {
    return fitX(fitWidth(b,bandSpan(b.y,b.h)),bandSpan(b.y,b.h));
}
constexpr Box fitVertical(Box b, int top, int bottom) {
    return fitY(fitHeight(b,top,bottom),top,bottom);
}
}
constexpr Box fitBox(Box b, Region region = Region::Content) {
    return detail::fitHorizontal(detail::fitVertical(b,
        region == Region::Content ? contentTop() : contentBoundary().y,
        contentBoundary().y+contentBoundary().h));
}
// Translate a fixed-size component as one unit. Never resize children; failure
// leaves the input untouched. Local drawing surfaces should skip physical fitting.
inline bool fitGroup(Box* boxes, unsigned count, Region region = Region::Content) {
    if (!boxes || !count) return false;
    int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
    for (unsigned i = 0; i < count; ++i) {
        const Box& b = boxes[i];
        if (b.w <= 0 || b.h <= 0 || b.x > INT_MAX - b.w || b.y > INT_MAX - b.h)
            return false;
        left = min(left, b.x); top = min(top, b.y);
        right = max(right, b.x + b.w); bottom = max(bottom, b.y + b.h);
    }
    const long long width = (long long)right - left, height = (long long)bottom - top;
    if (width > INT_MAX || height > INT_MAX) return false;
    const Box fitted = fitBox({left, top, (int)width, (int)height}, region);
    if (fitted.w != width || fitted.h != height) return false;
    for (unsigned i = 0; i < count; ++i) {
        boxes[i].x = fitted.x + (boxes[i].x - left);
        boxes[i].y = fitted.y + (boxes[i].y - top);
    }
    return true;
}
constexpr bool fits(Box b, Region region = Region::Content) {
    return b.w > 0 && b.h > 0 && (region == Region::Screen || b.y >= contentTop()) &&
           contains(contentBoundary(),b.x,b.y) && contains(contentBoundary(),b.x+b.w-1,b.y) &&
           contains(contentBoundary(),b.x,b.y+b.h-1) && contains(contentBoundary(),b.x+b.w-1,b.y+b.h-1);
}
}} // namespace ui::safe_area
