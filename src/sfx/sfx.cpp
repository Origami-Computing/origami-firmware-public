#include "sfx/sfx.h"

#include <Arduino.h>
#include <Preferences.h>

#include "services/media_volume.h"
#include "sfx/audio_group.h"
#include "sfx/sfx_voice.h"
#include "sfx/sfx_typewriter.h"

namespace sfx {
namespace {

constexpr char kPrefsNs[] = "sfxvol";
constexpr char kVolumeKey[] = "ui";

int s_volume = 80;
bool s_loaded = false;

int clampVolume(int v) {
    if (v < 0) return 0;
    if (v > 100) return 100;
    return v;
}

}  // namespace

void loadVolume() {
    if (s_loaded) return;
    Preferences p;
    if (p.begin(kPrefsNs, true)) {
        s_volume = clampVolume((int)p.getUChar(kVolumeKey, (uint8_t)s_volume));
        p.end();
    }
    s_loaded = true;
}

void saveVolume() {
    loadVolume();
    Preferences p;
    if (!p.begin(kPrefsNs, false)) {
        Serial.println("[sfx-vol] save failed: NVS open");
        return;
    }
    p.putUChar(kVolumeKey, (uint8_t)s_volume);
    p.end();
    Serial.printf("[sfx-vol] saved ui=%d\n", s_volume);
}

int volume() {
    loadVolume();
    return s_volume;
}

void setVolume(int vol0to100) {
    loadVolume();
    s_volume = clampVolume(vol0to100);
}

int scaledVolumeBasisPoints() {
    return services::audio::media_volume::scaledRouteVolumeBasisPoints(volume());
}

void play(const SfxDef& def) {
    if (!beginPlaybackSession()) return;

    SfxVoice voice;
    voice.start(def);
    while (voice.pump()) {
    }

    notePlaybackActivity(millis() + (uint32_t)(def.duration * 1000.f) + 8000u);
}

void onPlaybackRestored() {
    if (sfx::AudioGroup* active = sfx::activeAudioGroup()) {
        active->clear();
    }
    resetPlaybackSessionState();
    typewriterTeardown();
}

void service() {
    const uint32_t now = millis();

    if (sfx::AudioGroup* active = sfx::activeAudioGroup()) {
        active->service(now);
    }

    endPlaybackSessionIfIdle(now);
}

}  // namespace sfx
