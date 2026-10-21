#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "core/audio_dsp.h"

namespace core {
namespace audio {

// One visual response for every recording flow. Feed untouched 48 kHz I2S
// samples before WAV tuning or agent downsampling/gain. Windows follow sample
// time, so chunk size, UI frame rate and SD write timing cannot change decay.
class RecordingWaveform {
public:
    static constexpr int kBars = 27;
    static constexpr uint32_t kSamplesPerBar = 4800;  // 100 ms at 48 kHz

    void reset() {
        memset(history_, 0, sizeof(history_));
        samples_ = peak_ = revision_ = 0;
    }

    void ingestRaw(const int32_t* raw, size_t count) {
        if (!raw) return;
        for (size_t i = 0; i < count; ++i) {
            const int32_t sample = raw[i] >> 16;
            const uint32_t magnitude = sample < 0 ? -sample : sample;
            if (magnitude > peak_) peak_ = magnitude;
            if (++samples_ == kSamplesPerBar) {
                // Preserve the morning routine's quiet-speech lift and floor.
                const int16_t peakSample = peak_ == 32768 ? INT16_MIN : (int16_t)peak_;
                const uint8_t level = core::dsp::peakLevel(&peakSample, 1);
                memmove(history_, history_ + 1, kBars - 1);
                history_[kBars - 1] = level < 4 ? 0 : level;
                samples_ = peak_ = 0;
                ++revision_;
            }
        }
    }

    const uint8_t* data() const { return history_; }
    uint8_t level() const { return history_[kBars - 1]; }
    uint32_t revision() const { return revision_; }

private:
    uint8_t history_[kBars] = {};
    uint32_t samples_ = 0;
    uint32_t peak_ = 0;
    uint32_t revision_ = 0;
};

#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool checkRecordingWaveform();
#endif

}  // namespace audio
}  // namespace core
