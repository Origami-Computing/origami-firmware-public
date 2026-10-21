#include "scene/anim.h"

#include <math.h>

namespace scene {

float applyEase(Ease ease, float t) {
    if (t <= 0.f) return 0.f;
    if (t >= 1.f) return 1.f;
    switch (ease) {
        case Ease::Linear:
            return t;
        case Ease::EaseIn:
            return t * t;
        case Ease::EaseOut:
            return 1.f - (1.f - t) * (1.f - t);
        case Ease::EaseInOut: {
            if (t < 0.5f) return 2.f * t * t;
            return 1.f - powf(-2.f * t + 2.f, 2.f) / 2.f;
        }
        case Ease::SpringBounce:
            return 1.f - powf(1.f - t, 3.f) * cosf(t * 3.14159f * 2.5f);
        case Ease::StepInstant:
            return 1.f;
    }
    return t;
}

void AnimFloat::animate(float target, const Transition& tr, uint32_t now) {
    from     = value(now);
    to       = target;
    startMs  = now;
    duration = tr.durationMs;
    delay    = tr.delayMs;
    ease     = tr.ease;
    active   = true;
}

void AnimFloat::snap(float target) {
    cached = target;
    to     = target;
    active = false;
}

float AnimFloat::value(uint32_t now) const {
    if (!active) return cached;
    if (now < startMs + delay) return from;
    const uint32_t elapsed = now - startMs - delay;
    if (elapsed >= duration) return to;
    const float t  = (float)elapsed / (float)duration;
    const float et = applyEase(ease, t);
    return from + (to - from) * et;
}

bool AnimFloat::done(uint32_t now) const {
    if (!active) return true;
    return (now - startMs - delay) >= duration;
}

void AnimFloat::settle(uint32_t now) {
    if (active && done(now)) {
        cached = to;
        active = false;
    }
}

void FrameAnim::play(uint8_t start, uint8_t end, uint16_t durMs, bool lp, bool pp, uint32_t now) {
    frameStart = start;
    frameEnd   = end;
    frameDurMs = durMs;
    loop       = lp;
    pingPong   = pp;
    startMs    = now;
    active     = true;
}

void FrameAnim::snap(uint8_t frame) {
    frameStart = frame;
    frameEnd   = frame;
    active     = false;
}

uint8_t FrameAnim::currentFrame(uint32_t now) const {
    if (!active) return frameEnd;
    const uint8_t count = frameEnd - frameStart + 1;
    if (count == 0) return frameStart;
    uint32_t elapsed = now - startMs;
    uint32_t idx     = frameDurMs ? (elapsed / frameDurMs) : 0;
    if (pingPong) {
        const uint32_t period = (uint32_t)count * 2u - 2u;
        if (period == 0) return frameStart;
        idx = idx % period;
        if (idx >= count) idx = period - idx;
    } else if (loop) {
        idx = idx % count;
    } else if (idx >= count) {
        idx = count - 1;
    }
    return frameStart + (uint8_t)idx;
}

bool FrameAnim::done(uint32_t now) const {
    // After settle() clears active, the sequence is finished (mirrors AnimFloat::done).
    if (!active) return true;
    if (loop) return false;
    const uint8_t count = frameEnd - frameStart + 1;
    if (count == 0) return true;
    return (now - startMs) >= (uint32_t)count * frameDurMs;
}

void FrameAnim::settle(uint32_t now) {
    if (!loop && active && done(now)) active = false;
}

}  // namespace scene
