#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace core { namespace audio {

// Microphone recordings have substantially more headroom than mastered music.
// A bounded preamp with a peak envelope lifts speech without letting a brief
// handling transient determine the gain of the entire recording. This operates
// BEFORE media volume: it must never replace the speaker's 20% output ceiling.
// Single playback-worker ownership; reset at track start/restart, retain across
// chunks and pause/resume. No allocation, file scanning, or added AV latency.
class RecordingPlaybackDsp {
public:
    static constexpr float kMaxGain = 48.0f;
    static constexpr float kPeak = 27852.0f; // 85% of PCM full scale.

    void reset(uint32_t sampleRate) {
        envelope_ = 0.0f;
        release_ = std::exp(-1.0f / (0.150f * (sampleRate ? sampleRate : 48000)));
    }

    void process(int16_t* pcm, size_t frames, int channels) {
        if (!pcm || (channels != 1 && channels != 2)) return;
        for (size_t i = 0; i < frames; ++i) {
            float peak = 0.0f;
            for (int c = 0; c < channels; ++c) {
                const float magnitude = std::fabs(float(pcm[i * channels + c]));
                if (magnitude > peak) peak = magnitude;
            }
            envelope_ *= release_;
            if (peak > envelope_) envelope_ = peak;
            const float gain = envelope_ * kMaxGain > kPeak ? kPeak / envelope_ : kMaxGain;
            for (int c = 0; c < channels; ++c) {
                float value = pcm[i * channels + c] * gain;
                // Explicit bound protects against floating-point rounding too.
                if (value > kPeak) value = kPeak;
                if (value < -kPeak) value = -kPeak;
                pcm[i * channels + c] = int16_t(value);
            }
        }
    }

private:
    float envelope_ = 0.0f;
    float release_ = 1.0f;
};

} }
