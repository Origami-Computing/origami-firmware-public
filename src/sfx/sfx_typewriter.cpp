#include "sfx/sfx_typewriter.h"
#include <Arduino.h>

#include "hal/hal_audio.h"
#include "sfx/sfx.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace sfx {
namespace {

static bool s_open = false;
static uint32_t s_ownedTxGen = 0;
static uint32_t s_lastBlipMs = 0;
static bool s_hasBlip = false;
static TypewriterStats s_stats;
constexpr uint32_t kBlipIntervalMs = 56; // Greeting cadence, independent of text burst speed.

}  // namespace

bool typewriterOpen(uint32_t sampleRate) {
    if (s_open && hal::audio::txReady() && s_ownedTxGen == hal::audio::txGeneration()) return true;
    s_open = false;
    if (!hal::audio::configureForPlayback()) return false;
    if (!hal::audio::txBegin(sampleRate)) {
        hal::audio::restoreForUiPlayback();
        return false;
    }
    s_open = true;
    s_ownedTxGen = hal::audio::txGeneration();
    s_hasBlip = false;
    s_stats={};
    return true;
}

bool typewriterPlayChar(char c, size_t charIndex) {
    if (!s_open) return false;
    if (c == ' ' || c == '\n' || c == '\r' || c == '\t' || (unsigned char)c < 32u) return false;
    const uint32_t now = millis();
    if (s_hasBlip && now - s_lastBlipMs < kBlipIntervalMs) return false;

    // A4 / B4 / D5 / E5 / G5 — one octave below the original; small speakers reproduce
    // 440–784 Hz much more cleanly than 880–1568 Hz.
    static const float kHz[5] = {440.00f, 493.88f, 587.33f, 659.25f, 783.99f};
    const float        hz     = kHz[charIndex % 5u];
    constexpr float    kSr    = 16000.0f;

    constexpr int   kFrames       = 128;                // 8 ms @ 16 kHz — shorter blip, less blocking
    constexpr float kDuty         = 0.125f;             // 12.5 % pulse → digital nasal timbre
    constexpr float kAmp          = 0.90f;              // louder — low-freq needs more drive on small speakers
    constexpr int   kReleaseStart = (kFrames * 6) / 10; // last 40 % linear fade
    const float     deviceGain    = (float)scaledVolumeBasisPoints() / 10000.0f;

    int16_t     buf[kFrames * 2];
    float       phase = 0.0f;
    const float phInc = hz / kSr;
    for (int i = 0; i < kFrames; i++) {
        phase += phInc;
        if (phase >= 1.0f) phase -= 1.0f;
        const float pulse = (phase < kDuty) ? 1.0f : -1.0f;
        float       env   = 1.0f;
        if (i >= kReleaseStart) {
            env = 1.0f - (float)(i - kReleaseStart) / (float)(kFrames - kReleaseStart);
        }
        const int16_t s = (int16_t)(pulse * env * kAmp * deviceGain * 32767.0f);
        buf[i * 2]     = s;
        buf[i * 2 + 1] = s;
    }
    size_t written = 0;
    if (hal::audio::txWrite(buf, (size_t)(kFrames * 2), &written, pdMS_TO_TICKS(20)) == ESP_OK && written == sizeof(buf)) {
        // A blip is shorter than a DMA descriptor. Flush with silence so it
        // plays now instead of being joined to the next character's blip.
        hal::audio::txFinishBuffer(pdMS_TO_TICKS(20));
        if(s_hasBlip){const uint32_t gap=now-s_lastBlipMs;s_stats.totalGapMs+=gap;if(gap>s_stats.maxGapMs)s_stats.maxGapMs=gap;}
        ++s_stats.played;
        ++s_stats.noteCounts[charIndex % 5u];
        s_lastBlipMs = now;
        s_hasBlip = true;
        return true;
    }
    return false;
}

bool typewriterServiceChar(char c, size_t charIndex) {
    if (hal::audio::micReady()) {
        typewriterTeardown();
        return false;
    }
    if ((unsigned char)c <= 32u) return false;
    if (!typewriterOpen()) return false;
    return typewriterPlayChar(c, charIndex);
}

void typewriterTeardown() {
    if (!s_open) return;
    if (hal::audio::txReady() && s_ownedTxGen == hal::audio::txGeneration()) hal::audio::txEnd();
    s_open = false;
    s_hasBlip = false;
}

bool typewriterIsOpen() {
    return s_open && hal::audio::txReady() && s_ownedTxGen == hal::audio::txGeneration();
}
TypewriterStats typewriterStats(){return s_stats;}

}  // namespace sfx
