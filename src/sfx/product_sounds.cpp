#include "sfx/product_sounds.h"

#include <Arduino.h>
#include <esp_random.h>
#include <math.h>

#include "core/memory.h"
#include "hal/hal_audio.h"
#include "sfx/sfx.h"
#include "sfx/sfx_voice.h"

namespace sfx { namespace product {
namespace {
// Matches the shared SFX session rate so cues reuse an open TX session.
constexpr float kRate = 22050.f;
constexpr size_t kCapacity = 6000;   // longest recipe is ~0.25 s
constexpr int kChunkFrames = 512;
// Recipe levels are for a phone speaker (peaks near 0.09). The device speaker
// is small and its route profile scales UI volume (≈20% at full), so each cue
// is peak-normalised to the level of the existing UI blips before that scaling.
constexpr float kTargetPeak = 0.9f;
float* s_scratch = nullptr;          // retained PSRAM render buffer
int16_t* s_pcm = nullptr;            // retained PSRAM stereo chunk; I2S copies it to DMA
}

bool play(Sound sound) {
    if (hal::audio::micReady()) return false;   // caller may retry once the mic closes
    if (!s_scratch) {
        // One allocation: float render scratch followed by the int16 TX chunk.
        void* storage = core::memory::allocPsram("sfx.product",
            kCapacity * sizeof(float) + kChunkFrames * 2 * sizeof(int16_t));
        if (!storage) return false;
        s_scratch = static_cast<float*>(storage);
        s_pcm = reinterpret_cast<int16_t*>(s_scratch + kCapacity);
    }
    if (!beginPlaybackSession()) return false;

    const float pitch = 0.97f + (float)(esp_random() % 6001u) / 100000.f;
    const Recipe r = recipe(sound, pitch);
    // Typing repeats quickly; skip the reference's silent tail so cues never queue.
    const bool tail = sound != Sound::Typing;
    size_t count = frames(r, kRate, tail);
    if (count > kCapacity) count = kCapacity;
    render(r, kRate, ((uint64_t)esp_random() << 32) | esp_random() | 1u, s_scratch, count, tail);

    float peak = 0;
    for (size_t i = 0; i < count; ++i) { const float a = fabsf(s_scratch[i]); if (a > peak) peak = a; }
    const float gain = (peak > 0 ? kTargetPeak / peak : 0) * (float)scaledVolumeBasisPoints() / 10000.f;
    int16_t* const buf = s_pcm;
    for (size_t at = 0; at < count; at += kChunkFrames) {
        const size_t n = count - at < (size_t)kChunkFrames ? count - at : (size_t)kChunkFrames;
        for (size_t i = 0; i < n; ++i) {
            float v = s_scratch[at + i] * gain;
            if (v > 1.f) v = 1.f; else if (v < -1.f) v = -1.f;
            buf[i * 2] = buf[i * 2 + 1] = (int16_t)(v * 32767.f);
        }
        size_t written = 0;
        if (hal::audio::txWrite(buf, n * 2, &written, pdMS_TO_TICKS(40)) != ESP_OK || written != n * 2 * sizeof(int16_t)) break;
    }
    hal::audio::txFinishBuffer(pdMS_TO_TICKS(40));
    if (sound != Sound::Typing)
        Serial.printf("[sfx-product] sound=%u frames=%u peak=%.3f gain=%.2f\n", (unsigned)sound, (unsigned)count, peak, gain);
    notePlaybackActivity(millis() + (uint32_t)(count * 1000.f / kRate) + 8000u);
    return true;
}

}}
