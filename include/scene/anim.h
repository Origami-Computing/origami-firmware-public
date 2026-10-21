#pragma once

#include <stdint.h>

namespace scene {

enum class Ease : uint8_t {
    Linear,
    EaseIn,
    EaseOut,
    EaseInOut,
    SpringBounce,
    StepInstant,
};

struct Transition {
    uint16_t durationMs = 0;
    uint16_t delayMs    = 0;
    Ease     ease       = Ease::EaseOut;
};

inline Transition makeTransition(uint16_t durationMs, uint16_t delayMs, Ease ease) {
    Transition t;
    t.durationMs = durationMs;
    t.delayMs    = delayMs;
    t.ease       = ease;
    return t;
}

struct AnimFloat {
    float    from     = 0.f;
    float    to       = 0.f;
    uint32_t startMs  = 0;
    uint16_t duration = 0;
    uint16_t delay    = 0;
    Ease     ease     = Ease::EaseOut;
    bool     active   = false;
    float    cached   = 0.f;

    void animate(float target, const Transition& t, uint32_t now);
    void snap(float target);
    float value(uint32_t now) const;
    bool  done(uint32_t now) const;
    void  settle(uint32_t now);
};

struct FrameAnim {
    uint8_t  frameStart = 0;
    uint8_t  frameEnd   = 0;
    uint16_t frameDurMs = 0;
    bool     loop       = false;
    bool     pingPong   = false;
    bool     active     = false;
    uint32_t startMs    = 0;

    void play(uint8_t start, uint8_t end, uint16_t durMs, bool loop, bool pingPong, uint32_t now);
    void snap(uint8_t frame);
    uint8_t currentFrame(uint32_t now) const;
    bool    done(uint32_t now) const;
    void    settle(uint32_t now);
};

float applyEase(Ease ease, float t);

}  // namespace scene
