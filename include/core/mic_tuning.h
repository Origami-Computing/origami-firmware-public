#pragma once
/**
 * @file core/mic_tuning.h
 * @brief Live, persisted DSP controls for 48 kHz WAV microphone capture.
 */

#include <stddef.h>
#include <stdint.h>

namespace core {
namespace audio {
namespace mic_tuning {

struct Settings {
    uint16_t gainBasisPoints = 10000;  // 10000 = 1.00x; valid 0..40000.
    uint16_t highPassHz = 0;           // 0 = off; otherwise 20..500 Hz.
    int8_t limiterDbfs = 1;            // 1 = off; otherwise -20..0 dBFS.
};

// Settings/persistence and live processors are UI-task only.
void load();
Settings settings();
void setGainBasisPoints(int gainBasisPoints);
void setHighPassHz(int hz);
void setLimiterDbfs(int dbfs);  // Values outside -20..0 disable the limiter.
void resetDefaults();
bool save();
uint32_t generation();

class Processor {
public:
    void reset(uint32_t sampleRate = 48000);
    // Freeze validated DSP settings before handing this processor to one worker.
    // This mode never reads global settings/persistence; serialize instance use.
    void resetFromSnapshot(Settings snapshot, uint32_t sampleRate = 48000);
    void processRaw(const int32_t* raw, int16_t* pcm, size_t count);

    uint64_t samples() const { return samples_; }
    uint64_t sumSquares() const { return sumSquares_; }
    uint64_t sumAbs() const { return sumAbs_; }
    uint32_t peak() const { return peak_; }
    uint32_t overRangeSamples() const { return overRangeSamples_; }
    uint32_t limitedSamples() const { return limitedSamples_; }

private:
    void refreshSettings_();
    void resetState_(uint32_t sampleRate);
    void applySettings_(Settings snapshot);
    bool followLiveSettings_ = true;

    uint32_t sampleRate_ = 48000;
    uint32_t settingsGeneration_ = UINT32_MAX;
    int32_t alphaQ15_ = 0;
    int32_t limiterCeiling_ = 32767;
    int32_t previousInput_ = 0;
    int32_t previousOutput_ = 0;
    Settings active_{};

    uint64_t samples_ = 0;
    uint64_t sumSquares_ = 0;
    uint64_t sumAbs_ = 0;
    uint32_t peak_ = 0;
    uint32_t overRangeSamples_ = 0;
    uint32_t limitedSamples_ = 0;
};

}  // namespace mic_tuning
}  // namespace audio
}  // namespace core
