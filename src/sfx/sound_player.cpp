#include "sfx/sound_player.h"
#include <Arduino.h>

namespace sfx {
void initSoundPlayer(SoundPlayer& n) {
    n.playing  = false;
    n.repeating = false;
    n.repeatGapMs = 0;
    n.repeatAtMs = 0;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    n.startedCount = n.completedCount = n.renderedFrames = 0;
#endif
}

void SoundPlayer::playFromDef(const SfxDef& def, uint32_t now) {
    const uint16_t durMs = (uint16_t)(def.duration * 1000.f + 0.5f);
    playing = true;
    voice.start(def);
    if (!sfx::beginPlaybackSession()) {
        playing = false;
        voice.stop();
        return;
    }
    sfx::notePlaybackActivity(now + durMs + 8000u);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    ++startedCount;
#endif
}

void SoundPlayer::repeatFromDef(const SfxDef& def, uint16_t gapMs, uint32_t now) {
    repeatDef = def;
    repeatGapMs = gapMs;
    repeating = true;
    playFromDef(def, now);
}

void SoundPlayer::stop() {
    playing = false;
    repeating = false;
    voice.stop();
}

void SoundPlayer::service(uint32_t now) {
    if (!playing) {
        if (repeating && (int32_t)(now - repeatAtMs) >= 0) {
            playFromDef(repeatDef, now);
        }
        return;
    }
    // Prime two chunks on a new sound. Combined with the low-rate DMA profile,
    // this puts 35-46 ms ahead of the speaker before the UI draws the alert.
    const uint8_t chunks = voice.pos == 0 ? 2 : 1;
    for (uint8_t chunk = 0; chunk < chunks; ++chunk) {
        const int previousPosition = voice.pos;
        const bool more = voice.pump();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
        if (voice.pos > previousPosition) renderedFrames += voice.pos - previousPosition;
        if (!more && voice.pos >= (int)(voice.def.duration * 22050.f)) ++completedCount;
#else
        (void)previousPosition;
#endif
        if (more) {
            sfx::notePlaybackActivity(millis() + 8000u);
            continue;
        }
        playing = false;
        if (repeating) repeatAtMs = now + repeatGapMs;
        return;
    }
}

} // namespace sfx
