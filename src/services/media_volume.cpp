#include "services/media_volume.h"

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "hal/hal_audio.h"

namespace services {
namespace audio {
namespace media_volume {

namespace {
constexpr char kPrefsNs[] = "mp3vol2";
constexpr char kVolumeKey[] = "ui";
constexpr char kSpeakerMinKey[] = "sp_min_bp";
constexpr char kSpeakerMaxKey[] = "sp_max_bp";
constexpr char kSpeakerStepsKey[] = "sp_steps";
constexpr char kHeadphoneMinKey[] = "hp_min_bp";
constexpr char kHeadphoneMaxKey[] = "hp_max_bp";
constexpr char kHeadphoneStepsKey[] = "hp_steps";

struct State {
    VolumeProfile speaker{100, kSpeakerMaxGainBasisPoints, 16};
    VolumeProfile headphone{100, 3000, 16};
    int volume = 80;
    int overrideGain = -1;
};
State s_state;
int s_savedVolume = 80;
std::atomic<unsigned> s_loadState{0}; // 0 uninitialized, 1 reading NVS, 2 published
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
struct StateGuard {
    StateGuard() { portENTER_CRITICAL(&s_mux); }
    ~StateGuard() { portEXIT_CRITICAL(&s_mux); }
};
State snapshot() {
    StateGuard guard;
    return s_state;
}

int clampVolume(int value) {
    if (value < 0) return 0;
    if (value > 100) return 100;
    return value;
}

uint16_t clampBasisPoints(int value, uint16_t fallback) {
    if (value < 0) return fallback;
    if (value > 10000) return 10000;
    return (uint16_t)value;
}

uint8_t clampSteps(int value, uint8_t fallback) {
    if (value < 1) return fallback;
    if (value > 100) return 100;
    return (uint8_t)value;
}

VolumeProfile normalizeProfile(VolumeProfile profile, uint16_t ceiling = 10000) {
    if (profile.maxGainBasisPoints > ceiling) profile.maxGainBasisPoints = ceiling;
    if (profile.maxGainBasisPoints > 0 && profile.minGainBasisPoints == 0) {
        profile.minGainBasisPoints = 1;
    }
    if (profile.minGainBasisPoints > profile.maxGainBasisPoints) {
        profile.minGainBasisPoints = profile.maxGainBasisPoints;
    }
    if (profile.steps < 1) profile.steps = 1;
    if (profile.steps > 100) profile.steps = 100;
    return profile;
}

int applyProfile(int uiVolume, VolumeProfile profile) {
    if (uiVolume <= 0 || profile.maxGainBasisPoints == 0) return 0;
    uiVolume = clampVolume(uiVolume);
    profile = normalizeProfile(profile);
    if (profile.steps <= 1) return profile.maxGainBasisPoints;

    const int stepIndex = ((uiVolume - 1) * (int)profile.steps) / 100;
    const float t = (float)stepIndex / (float)(profile.steps - 1);
    const float gain = (float)profile.minGainBasisPoints *
        powf((float)profile.maxGainBasisPoints /
                 (float)profile.minGainBasisPoints,
             t);
    int rounded = (int)(gain + 0.5f);
    if (rounded <= 0) rounded = 1;
    if (rounded > 10000) rounded = 10000;
    return rounded;
}
}  // namespace

void load() {
    if (s_loadState.load(std::memory_order_acquire) == 2) return;
    unsigned expected = 0;
    if (!s_loadState.compare_exchange_strong(expected, 1)) {
        while (s_loadState.load(std::memory_order_acquire) != 2)
            vTaskDelay(pdMS_TO_TICKS(1));
        return;
    }
    State loaded = snapshot();

    Preferences p;
    if (p.begin(kPrefsNs, true)) {
        loaded.volume = clampVolume((int)p.getUChar(kVolumeKey, (uint8_t)loaded.volume));
        loaded.speaker.minGainBasisPoints = clampBasisPoints(
            p.getUShort(kSpeakerMinKey, loaded.speaker.minGainBasisPoints),
            loaded.speaker.minGainBasisPoints);
        loaded.speaker.maxGainBasisPoints = clampBasisPoints(
            p.getUShort(kSpeakerMaxKey, loaded.speaker.maxGainBasisPoints),
            loaded.speaker.maxGainBasisPoints);
        loaded.speaker.steps = clampSteps(
            p.getUChar(kSpeakerStepsKey, loaded.speaker.steps),
            loaded.speaker.steps);
        loaded.headphone.minGainBasisPoints = clampBasisPoints(
            p.getUShort(kHeadphoneMinKey, loaded.headphone.minGainBasisPoints),
            loaded.headphone.minGainBasisPoints);
        loaded.headphone.maxGainBasisPoints = clampBasisPoints(
            p.getUShort(kHeadphoneMaxKey, loaded.headphone.maxGainBasisPoints),
            loaded.headphone.maxGainBasisPoints);
        loaded.headphone.steps = clampSteps(
            p.getUChar(kHeadphoneStepsKey, loaded.headphone.steps),
            loaded.headphone.steps);
        p.end();
    }

    loaded.speaker = normalizeProfile(loaded.speaker, kSpeakerMaxGainBasisPoints);
    loaded.headphone = normalizeProfile(loaded.headphone);
    {
        StateGuard guard;
        s_state.speaker = loaded.speaker;
        s_state.headphone = loaded.headphone;
        s_state.volume = s_savedVolume = loaded.volume;
    }
    s_loadState.store(2, std::memory_order_release);
}

int volume() {
    load();
    return snapshot().volume;
}

void setVolume(int vol0to100) {
    load();
    StateGuard guard;
    s_state.volume = clampVolume(vol0to100);
}

void saveVolume() {
    load();
    int value;
    {
        StateGuard guard;
        value = s_state.volume;
        if (value == s_savedVolume) return;
    }

    Preferences p;
    if (!p.begin(kPrefsNs, false)) {
        Serial.println("[media-vol] save failed: NVS open");
        return;
    }
    const size_t written = p.putUChar(kVolumeKey, (uint8_t)value);
    p.end();
    if (written == 0) {
        Serial.println("[media-vol] save failed: NVS write");
        return;
    }
    { StateGuard guard; s_savedVolume = value; }
    Serial.printf("[media-vol] saved ui=%d\n", value);
}

VolumeProfile speakerProfile() {
    load();
    return snapshot().speaker;
}

VolumeProfile headphoneProfile() {
    load();
    return snapshot().headphone;
}

void setSpeakerProfile(VolumeProfile profile) {
    load();
    profile = normalizeProfile(profile, kSpeakerMaxGainBasisPoints);
    StateGuard guard;
    s_state.speaker = profile;
}

void setHeadphoneProfile(VolumeProfile profile) {
    load();
    profile = normalizeProfile(profile);
    StateGuard guard;
    s_state.headphone = profile;
}

void saveProfiles() {
    load();
    const State state = snapshot();
    Preferences p;
    if (!p.begin(kPrefsNs, false)) {
        Serial.println("[media-vol] profile save failed: NVS open");
        return;
    }
    p.putUShort(kSpeakerMinKey, state.speaker.minGainBasisPoints);
    p.putUShort(kSpeakerMaxKey, state.speaker.maxGainBasisPoints);
    p.putUChar(kSpeakerStepsKey, state.speaker.steps);
    p.putUShort(kHeadphoneMinKey, state.headphone.minGainBasisPoints);
    p.putUShort(kHeadphoneMaxKey, state.headphone.maxGainBasisPoints);
    p.putUChar(kHeadphoneStepsKey, state.headphone.steps);
    p.end();
    Serial.println("[media-vol] profiles saved");
}

int scaledRouteVolumeBasisPoints(int vol0to100) {
    load();
    const State state = snapshot();
    const bool speaker = hal::audio::getEffectiveOutputRoute() != hal::audio::OutputRoute::HeadphonesOnly;
    return applyProfile(vol0to100, speaker ? state.speaker : state.headphone);
}

int scaledVolumeBasisPoints() {
    load();
    const State state = snapshot();
    const bool speaker = hal::audio::getEffectiveOutputRoute() != hal::audio::OutputRoute::HeadphonesOnly;
    if (state.overrideGain >= 0) return speaker && state.overrideGain > kSpeakerMaxGainBasisPoints
        ? kSpeakerMaxGainBasisPoints : state.overrideGain;
    return applyProfile(state.volume, speaker ? state.speaker : state.headphone);
}

void setGainOverrideBasisPoints(int gainBasisPoints) {
    if (gainBasisPoints < 0) gainBasisPoints = -1;
    if (gainBasisPoints > 10000) gainBasisPoints = 10000;
    StateGuard guard;
    s_state.overrideGain = gainBasisPoints;
}

int gainOverrideBasisPoints() {
    return snapshot().overrideGain;
}

}  // namespace media_volume
}  // namespace audio
}  // namespace services
