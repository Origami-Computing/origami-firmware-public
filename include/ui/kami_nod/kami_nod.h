#pragma once
#include <algorithm>
#include <cmath>
#include "ui/raster.h"
#include "kami_nod_spans.h"

namespace kami_nod {
enum class Easing { Smoothstep, Linear, Quadratic };
struct Settings {
    int sizePercent;
    int speedPercent;
    int eyeFollowPercent;
    int bodyTravelPercent;
    int eyeTravelPercent;
    Easing easing;
};
static constexpr Settings defaults{125, 100, 75, 100, 100, Easing::Smoothstep};

namespace detail {
template<class Canvas, size_t Rows>
void layer(Canvas& canvas, const Span (&spans)[Rows], int width, int height,
           float x, float y, float scaleX, float scaleY, uint16_t ink) {
    const int left = std::max(0, int(std::floor(x)));
    const int right = std::min(width, int(std::ceil(x + 240*scaleX)));
    const int top = std::max(0, int(std::floor(y)));
    const int bottom = std::min(height, int(std::ceil(y + Rows*scaleY)));
    for (int py = top; py < bottom; ++py) {
        float starts[4], ends[4];
        float rowLeft = float(right), rowRight = float(left);
        for (int sample = 0; sample < 4; ++sample) {
            const float sourceY = (py + (sample + 0.5f)/4 - y)/scaleY;
            starts[sample] = ends[sample] = x;
            if (sourceY < 0 || sourceY >= Rows) continue;
            const Span span = spans[int(sourceY)];
            if (span.left == span.right) continue;
            starts[sample] = x + span.left*scaleX/16;
            ends[sample] = x + span.right*scaleX/16;
            rowLeft = std::min(rowLeft, starts[sample]);
            rowRight = std::max(rowRight, ends[sample]);
        }
        for (int px = std::max(left, int(std::floor(rowLeft)));
             px < std::min(right, int(std::ceil(rowRight))); ++px) {
            float coverage = 0;
            for (int sample = 0; sample < 4; ++sample)
                coverage += std::max(0.0f, std::min(float(px+1), ends[sample]) -
                                           std::max(float(px), starts[sample]));
            const int alpha = int(coverage*3.75f + 0.5f);
            if (alpha)
                canvas.fillRect(px, py, 1, 1, alpha == 15 ? ink :
                                ui::raster::blend(ink, canvas.at(px, py), alpha));
        }
    }
}
} // namespace detail

// Composite onto a freshly rendered RGB565 background each frame.
// Canvas supplies at(x,y) and fillRect(x,y,w,h,color). Width/height bound reads.
// x/y are the neutral body's top left. Base width is 24 pixels at 100%.
template<class Canvas>
void render(Canvas& canvas, int width, int height, float x, float y,
            uint32_t elapsedMs, const Settings& settings = defaults) {
    if (settings.sizePercent <= 0) return;
    constexpr int time[] = {0, 300, 550, 950, 1225, 1800};
    constexpr int tops[] = {68, 70, 70, 67, 67, 68};
    constexpr int heights[] = {26, 24, 24, 27, 27, 26};
    constexpr int eyes[] = {79, 79, 81, 81, 78, 79};
    const float t = float((uint64_t(elapsedMs)*std::max(0,settings.speedPercent)) % 180000)/100;
    int frame = 0;
    while (t >= time[frame+1]) ++frame;
    float u = (t-time[frame])/float(time[frame+1]-time[frame]);
    if (settings.easing == Easing::Smoothstep) u = u*u*(3-2*u);
    if (settings.easing == Easing::Quadratic)
        u = u < 0.5f ? 2*u*u : 1-2*(1-u)*(1-u);
    const auto tween = [frame,u](const int* track) {
        return track[frame]+(track[frame+1]-track[frame])*u;
    };
    const float scale = settings.sizePercent/1000.0f;
    const float bodyAmount = settings.bodyTravelPercent/100.0f;
    const float bodyOffset = (tween(tops)-68)*10*bodyAmount;
    const float bodyScale = 1+(tween(heights)/26-1)*bodyAmount;
    const float tracked = bodyOffset+125*(bodyScale-1);
    const float original = (tween(eyes)-79)*10;
    const float eyeOffset = (original+(tracked-original)*settings.eyeFollowPercent/100)
                            *settings.eyeTravelPercent/100;
    detail::layer(canvas, detail::body, width, height, x, y+bodyOffset*scale,
                  scale, scale*bodyScale, 0xf945);
    detail::layer(canvas, detail::leftEye, width, height, x, y+eyeOffset*scale,
                  scale, scale, 0x2104);
    detail::layer(canvas, detail::rightEye, width, height, x, y+eyeOffset*scale,
                  scale, scale, 0x2104);
}
} // namespace kami_nod
