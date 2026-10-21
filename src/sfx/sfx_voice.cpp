#include "sfx/sfx_voice.h"

#include <Arduino.h>
#include <cmath>

#include "hal/hal_audio.h"

namespace sfx {
namespace {

constexpr uint32_t kSampleRate       = 22050;
// 512 frames provide 23.2 ms per feed at 22.05 kHz. The old 128-frame feed
// covered only 5.8 ms, shorter than a normal alert-screen draw/flush pass.
constexpr int      kChunkFrames      = 512;
constexpr uint32_t kTeardownMarginMs = 8000;

static uint32_t s_noiseLfsr = 0xACE1u;

static bool     s_sessionActive     = false;
static uint32_t s_doneAtMs          = 0;
static uint32_t s_ownedTxGen        = 0;
static bool     s_playbackConfigured = false;
static uint32_t s_seenCodecGen      = 0;

static float oscillator(SfxWaveform wave, float phase) {
    switch (wave) {
        case SFX_SQUARE:
            return (phase < 0.5f) ? 1.0f : -1.0f;
        case SFX_SAW:
            return 2.0f * phase - 1.0f;
        case SFX_SINE:
            return sinf(phase * 2.0f * (float)M_PI);
        case SFX_NOISE:
            s_noiseLfsr = (s_noiseLfsr >> 1) ^ (-(s_noiseLfsr & 1u) & 0xB400u);
            return ((float)(s_noiseLfsr & 0xFFFF) / 32768.0f) - 1.0f;
    }
    return 0.f;
}

static float freqAt(const SfxDef& def, float t) {
    if (def.arp1 > 0.f && def.arp2 > 0.f) {
        if (t < 0.333f) return def.freqStart;
        if (t < 0.666f) return def.arp1;
        return def.arp2;
    }
    return def.freqStart + (def.freqEnd - def.freqStart) * t;
}

static float envelopeAt(const SfxDef& def, float t) {
    if (t < def.attack) return t / def.attack;
    if (t > (1.0f - def.decay)) return (1.0f - t) / def.decay;
    return 1.0f;
}

}  // namespace

bool ensurePlaybackCodec() {
    const uint32_t codecGen = hal::audio::codecConfigGeneration();
    if (s_seenCodecGen != codecGen) {
        s_playbackConfigured = false;
        s_seenCodecGen = codecGen;
    }
    if (!hal::audio::codecReady()) {
        hal::audio::hardwareReset();
        if (!hal::audio::initCodec(0x18)) return false;
        s_playbackConfigured = false;
        s_seenCodecGen = hal::audio::codecConfigGeneration();
    }
    if (!s_playbackConfigured) {
        if (!hal::audio::configureForPlayback()) return false;
        s_playbackConfigured = true;
        s_seenCodecGen = hal::audio::codecConfigGeneration();
    }
    return true;
}

bool beginPlaybackSession() {
    if (hal::audio::micReady()) return false;
    if (hal::audio::txReady()) {
        if (s_sessionActive && s_ownedTxGen == hal::audio::txGeneration()) return true;
        return false;
    }
    if (!ensurePlaybackCodec()) return false;
    if (!hal::audio::txBegin(kSampleRate)) return false;
    s_noiseLfsr     = 0xACE1u;
    s_ownedTxGen    = hal::audio::txGeneration();
    s_sessionActive = true;
    return true;
}

void notePlaybackActivity(uint32_t doneAtMs) {
    s_doneAtMs = doneAtMs;
}

void endPlaybackSessionIfIdle(uint32_t now) {
    if (!s_sessionActive) return;
    if (s_ownedTxGen != hal::audio::txGeneration()) {
        s_sessionActive = false;
        s_playbackConfigured = false;
        s_seenCodecGen = hal::audio::codecConfigGeneration();
        return;
    }
    if ((int32_t)(now - s_doneAtMs) >= 0) {
        hal::audio::txEnd();
        s_sessionActive = false;
        s_playbackConfigured = false;
    }
}

void resetPlaybackSessionState() {
    s_sessionActive = false;
    s_doneAtMs = 0;
    s_ownedTxGen = 0;
    s_playbackConfigured = false;
    s_seenCodecGen = hal::audio::codecConfigGeneration();
}

void SfxVoice::start(const SfxDef& defIn) {
    def    = defIn;
    pos    = 0;
    phase  = 0.f;
    active = true;
}

void SfxVoice::stop() {
    active = false;
    pos    = 0;
}

bool SfxVoice::pump() {
    if (!active) return false;

    const int totalFrames = (int)(def.duration * (float)kSampleRate);
    if (pos >= totalFrames) {
        active = false;
        return false;
    }

    const int n = ((totalFrames - pos) < kChunkFrames) ? (totalFrames - pos) : kChunkFrames;
    const float deviceGain = (float)scaledVolumeBasisPoints() / 10000.0f;
    // SFX run on the single UI/audio owner task. Static scratch avoids spending
    // another 2 KB of its deliberately tight stack on every pump call.
    alignas(4) static int16_t buf[kChunkFrames * 2];

    for (int i = 0; i < n; i++) {
        const float t     = (float)(pos + i) / (float)totalFrames;
        const float freq  = freqAt(def, t);
        const float env   = envelopeAt(def, t) * def.volume * deviceGain;
        const float sample = oscillator(def.wave, phase) * env;
        const int16_t s16  = (int16_t)(sample * 32767.0f);
        buf[i * 2]         = s16;
        buf[i * 2 + 1]     = s16;
        phase += freq / (float)kSampleRate;
        if (phase >= 1.0f) phase -= 1.0f;
    }

    size_t written = 0;
    const size_t expectedBytes = (size_t)n * 2u * sizeof(int16_t);
    const esp_err_t err =
        hal::audio::txWrite(buf, (size_t)(n * 2), &written, pdMS_TO_TICKS(40));
    if (err != ESP_OK || written != expectedBytes) {
        // Do not advance phase/position past audio the driver did not accept.
        // A clean stop is preferable to resuming from a discontinuous fragment.
        hal::audio::txFinishBuffer(pdMS_TO_TICKS(40));
        active = false;
        return false;
    }
    pos += n;

    if (pos >= totalFrames) {
        // IDF 4.4 retains a partial DMA-buffer offset across calls. Completing
        // the buffer prevents a repeated alarm from being split into pulses.
        hal::audio::txFinishBuffer(pdMS_TO_TICKS(40));
        active = false;
        return false;
    }
    return true;
}

}  // namespace sfx
