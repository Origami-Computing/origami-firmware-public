#pragma once
#include "sfx/sfx_voice.h"

namespace sfx {
// UI-task scheduled voice/repeat playback. No visual state or worker of its own.
struct SoundPlayer {
    bool        playing  = false;
    bool        repeating = false;
    uint16_t    repeatGapMs = 0;
    uint32_t    repeatAtMs = 0;
    SfxDef      repeatDef = {};
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    uint32_t startedCount = 0, completedCount = 0, renderedFrames = 0;
#endif

    sfx::SfxVoice voice;

    void playFromDef(const SfxDef& def, uint32_t now);
    void repeatFromDef(const SfxDef& def, uint16_t gapMs, uint32_t now);
    void stop();
    /** Called from sfx::service() / AudioGroup::service(). */
    void service(uint32_t now);
};

// Reset scheduling/counters only; callers must stop an active voice separately.
void initSoundPlayer(SoundPlayer& player);
} // namespace sfx
