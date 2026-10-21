#include "core/mic_tuning.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>

namespace core {
namespace audio {
namespace mic_tuning {

namespace {
constexpr char kPrefsNs[] = "mictune";
constexpr char kGainKey[] = "gain_bp";
constexpr char kHighPassKey[] = "hpf_hz";
constexpr char kLimiterKey[] = "limit_db";

Settings s_settings{};
bool s_loaded = false;
uint32_t s_generation = 1;

uint16_t clampGain(int value) {
    if (value < 0) return 0;
    if (value > 40000) return 40000;
    return (uint16_t)value;
}

uint16_t clampHighPass(int value) {
    if (value == 0) return 0;
    if (value < 20) return 20;
    if (value > 500) return 500;
    return (uint16_t)value;
}

int8_t clampLimiter(int value) {
    if (value < -20 || value > 0) return 1;
    return (int8_t)value;
}

void noteChanged() {
    ++s_generation;
    if (s_generation == 0) s_generation = 1;
}
}  // namespace

void load() {
    if (s_loaded) return;
    Preferences p;
    if (p.begin(kPrefsNs, true)) {
        s_settings.gainBasisPoints = clampGain(
            (int)p.getUShort(kGainKey, s_settings.gainBasisPoints));
        s_settings.highPassHz = clampHighPass(
            (int)p.getUShort(kHighPassKey, s_settings.highPassHz));
        s_settings.limiterDbfs = clampLimiter(
            (int)p.getChar(kLimiterKey, s_settings.limiterDbfs));
        p.end();
    }
    s_loaded = true;
}

Settings settings() {
    load();
    return s_settings;
}

void setGainBasisPoints(int gainBasisPoints) {
    load();
    const uint16_t value = clampGain(gainBasisPoints);
    if (value == s_settings.gainBasisPoints) return;
    s_settings.gainBasisPoints = value;
    noteChanged();
}

void setHighPassHz(int hz) {
    load();
    const uint16_t value = clampHighPass(hz);
    if (value == s_settings.highPassHz) return;
    s_settings.highPassHz = value;
    noteChanged();
}

void setLimiterDbfs(int dbfs) {
    load();
    const int8_t value = clampLimiter(dbfs);
    if (value == s_settings.limiterDbfs) return;
    s_settings.limiterDbfs = value;
    noteChanged();
}

void resetDefaults() {
    load();
    s_settings = Settings{};
    noteChanged();
}

bool save() {
    load();
    Preferences p;
    if (!p.begin(kPrefsNs, false)) return false;
    const bool ok = p.putUShort(kGainKey, s_settings.gainBasisPoints) > 0 &&
                    p.putUShort(kHighPassKey, s_settings.highPassHz) > 0 &&
                    p.putChar(kLimiterKey, s_settings.limiterDbfs) > 0;
    p.end();
    return ok;
}

uint32_t generation() {
    load();
    return s_generation;
}

void Processor::reset(uint32_t sampleRate) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000;
    settingsGeneration_ = UINT32_MAX;
    previousInput_ = 0;
    previousOutput_ = 0;
    samples_ = 0;
    sumSquares_ = 0;
    sumAbs_ = 0;
    peak_ = 0;
    overRangeSamples_ = 0;
    limitedSamples_ = 0;
    refreshSettings_();
}

void Processor::refreshSettings_() {
    const uint32_t currentGeneration = generation();
    if (settingsGeneration_ == currentGeneration) return;
    active_ = settings();
    settingsGeneration_ = currentGeneration;

    if (active_.highPassHz == 0) {
        alphaQ15_ = 0;
        previousInput_ = 0;
        previousOutput_ = 0;
    } else {
        const float alpha = (float)sampleRate_ /
            ((float)sampleRate_ + 2.0f * (float)M_PI * active_.highPassHz);
        alphaQ15_ = (int32_t)(alpha * 32768.0f + 0.5f);
    }

    limiterCeiling_ = active_.limiterDbfs <= 0
        ? (int32_t)(32767.0f * powf(10.0f, (float)active_.limiterDbfs / 20.0f) + 0.5f)
        : 32767;
}

void Processor::processRaw(const int32_t* raw, int16_t* pcm, size_t count) {
    if (!raw || !pcm || count == 0) return;
    refreshSettings_();

    for (size_t i = 0; i < count; ++i) {
        const int32_t input = raw[i] >> 16;
        int32_t filtered = input;
        if (active_.highPassHz > 0) {
            const int64_t next = (int64_t)alphaQ15_ *
                ((int64_t)previousOutput_ + input - previousInput_);
            filtered = (int32_t)(next / 32768);
            previousInput_ = input;
            previousOutput_ = filtered;
        }

        int32_t value = (int32_t)(((int64_t)filtered * active_.gainBasisPoints) / 10000);
        if (value > 32767 || value < -32768) ++overRangeSamples_;

        if (active_.limiterDbfs <= 0) {
            if (value > limiterCeiling_) {
                value = limiterCeiling_;
                ++limitedSamples_;
            } else if (value < -limiterCeiling_) {
                value = -limiterCeiling_;
                ++limitedSamples_;
            }
        }
        if (value > 32767) value = 32767;
        if (value < -32768) value = -32768;

        pcm[i] = (int16_t)value;
        const int32_t magnitude = value < 0 ? -value : value;
        if ((uint32_t)magnitude > peak_) peak_ = (uint32_t)magnitude;
        sumAbs_ += (uint32_t)magnitude;
        sumSquares_ += (uint64_t)((int64_t)value * value);
        ++samples_;
    }
}

}  // namespace mic_tuning
}  // namespace audio
}  // namespace core
