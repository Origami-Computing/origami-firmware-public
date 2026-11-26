#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>

// Product sound recipes shared with the phone prototype (ProductSound /
// desk-sounds.ts). Recipes are data; render() is a pure function so host tests
// can compare it with the reference. play() is the only effect and lives in
// src/sfx/product_sounds.cpp on top of the existing SFX playback session.
namespace sfx { namespace product {

enum class Sound : uint8_t { WaterDrop, AddMenu, Typing, TalkStart, Send, Cancel, Lock, LibraryOpen, LibraryClose };

struct Voice {
    float frequency;
    float end = 0;        // 0 = no sweep
    float delay = 0;
    float attack = 0.006f;
    float decay = 0.12f;
    float gain = 0.1f;
    float cutoff = 1000;  // 0 = unfiltered
    float quality = 0.5f; // WebAudio low-pass Q, in dB
    bool noise = false;
};

struct Recipe { Voice voices[2]; uint8_t count; };

inline Voice voice(float f) { Voice v{}; v.frequency = f; return v; }

inline Recipe recipe(Sound sound, float j) {
    Recipe r{}; r.count = 1;
    Voice& a = r.voices[0]; Voice& b = r.voices[1];
    switch (sound) {
    case Sound::WaterDrop: a = voice(700*j); a.end = 1100*j; a.attack = .003f; a.decay = .04f; a.gain = .15f; a.cutoff = 0; break;
    case Sound::AddMenu:   a = voice(420*j); a.end = 760*j;  a.attack = .003f; a.decay = .07f; a.gain = .13f; a.cutoff = 0; break;
    case Sound::Typing:
        a = voice(900*j); a.attack = .001f; a.decay = .006f; a.gain = .05f; a.quality = .8f; a.noise = true;
        b = voice(220*j); b.attack = .003f; b.decay = .035f; b.gain = .05f; b.cutoff = 700; r.count = 2; break;
    case Sound::TalkStart:
        a = voice(392*j); a.decay = .08f; a.gain = .07f;
        b = voice(523*j); b.delay = .06f; b.gain = .06f; r.count = 2; break;
    case Sound::Send: case Sound::Cancel:
        a = voice(300*j); a.end = 480*j; a.attack = .003f; a.decay = .03f; a.gain = .05f; a.cutoff = 1200;
        b = voice(700*j); b.end = 900*j; b.delay = .015f; b.attack = .01f; b.decay = .2f; b.gain = .016f; b.quality = .6f; b.noise = true;
        r.count = 2; break;
    case Sound::Lock:
        a = voice(150*j); a.end = 300*j; a.attack = .004f; a.decay = .06f; a.gain = .14f; a.cutoff = 900;
        b = voice(75*j); b.attack = .004f; b.decay = .05f; b.gain = .06f; b.cutoff = 400; r.count = 2; break;
    case Sound::LibraryOpen:
        a = voice(140*j); a.end = 112*j; a.delay = .06f; a.decay = .13f; a.gain = .15f; a.cutoff = 600;
        b = voice(70*j); b.delay = .06f; b.gain = .07f; b.cutoff = 300; r.count = 2; break;
    case Sound::LibraryClose:
        a = voice(125*j); a.end = 100*j; a.decay = .1f; a.gain = .11f; a.cutoff = 550;
        b = voice(62*j); b.decay = .09f; b.gain = .05f; b.cutoff = 300; r.count = 2; break;
    }
    return r;
}

// Frames covering every voice. The reference adds a 50 ms tail after each
// envelope; tail=false stops where the envelope reaches its floor so short
// cues such as Typing can repeat every 56 ms without queueing behind silence.
inline size_t frames(const Recipe& r, float rate, bool tail = true) {
    float duration = 0;
    for (uint8_t i = 0; i < r.count; ++i) {
        const Voice& v = r.voices[i];
        const float d = v.delay + v.attack + v.decay + (tail ? .05f : .002f);
        if (d > duration) duration = d;
    }
    return (size_t)ceilf(duration * rate);
}

// Mixes the recipe into out[0..count). Noise uses the reference's 64-bit LCG.
// Single precision throughout: the ESP32-S3 FPU has no double support, and a
// double renderer stalled the UI task for over a second per cue. Phase wraps
// each sample so float accumulation stays exact enough for the reference test.
inline void render(const Recipe& r, float rate, uint64_t seed, float* out, size_t count, bool tail = true) {
    constexpr float kTwoPi = 6.28318530718f;
    for (size_t i = 0; i < count; ++i) out[i] = 0;
    uint64_t random = seed;
    for (uint8_t n = 0; n < r.count; ++n) {
        const Voice& v = r.voices[n];
        float phase = 0, x1 = 0, x2 = 0, y1 = 0, y2 = 0;
        const size_t start = (size_t)(v.delay * rate);
        const size_t length = (size_t)((v.attack + v.decay + (tail ? .05f : .002f)) * rate);
        const float endHz = v.end > 0 ? v.end : v.frequency;
        const float ratio = endHz / v.frequency;
        const float ramp = v.cutoff <= 0 ? v.decay : v.attack + v.decay;
        const float q = 2.f * powf(10.f, v.quality / 20.f);
        // Fixed-cutoff voices reuse one set of filter coefficients.
        const float fixedOmega = kTwoPi * v.cutoff / rate;
        const float fixedC = cosf(fixedOmega), fixedAlpha = sinf(fixedOmega) / q;
        const float floorRatio = 0.0001f / v.gain;
        for (size_t i = 0; i < length && start + i < count; ++i) {
            const float t = (float)i / rate;
            const float frequency = ratio == 1.f ? v.frequency : v.frequency * powf(ratio, fminf(1.f, t / ramp));
            random = random * 6364136223846793005ULL + 1ULL;
            float sample = v.noise ? (float)((double)(random >> 11) / (double)(UINT64_MAX >> 11)) * 2.f - 1.f : sinf(phase);
            phase += kTwoPi * frequency / rate;
            if (phase >= kTwoPi) phase -= kTwoPi;
            if (v.cutoff > 0) {
                float c = fixedC, alpha = fixedAlpha;
                if (v.noise) { const float omega = kTwoPi * frequency / rate; c = cosf(omega); alpha = sinf(omega) / q; }
                const float b0 = (1 - c) / 2, b1 = 1 - c, a0 = 1 + alpha;
                const float y = (b0 * sample + b1 * x1 + b0 * x2 + 2 * c * y1 - (1 - alpha) * y2) / a0;
                x2 = x1; x1 = sample; y2 = y1; y1 = y; sample = y;
            }
            const float envelope = t < v.attack ? v.gain * t / v.attack
                : v.gain * powf(floorRatio, fminf(1.f, (t - v.attack) / v.decay));
            out[start + i] += sample * envelope * 0.6f;
        }
    }
}

// UI task only. Plays through the shared SFX session with ±3% pitch variation.
// Returns false without blocking when the microphone or another owner holds
// I2S. Blocks only while the rendered cue (≤ 0.3 s) is queued to DMA.
bool play(Sound sound);

}}
