#pragma once

#include <stdint.h>

#include "sfx/sfx.h"

namespace sfx {

/** One-shot voice renderer — shared by sfx::play() and sfx::SoundPlayer. */
struct SfxVoice {
    SfxDef  def     = {};
    int     pos     = 0;
    float   phase   = 0.f;
    bool    active  = false;

    void start(const SfxDef& def);
    /** Render up to one chunk; returns true while more samples remain. */
    bool pump();
    void stop();
};

bool ensurePlaybackCodec();
void notePlaybackActivity(uint32_t doneAtMs);
bool beginPlaybackSession();
void endPlaybackSessionIfIdle(uint32_t now);
void resetPlaybackSessionState();

}  // namespace sfx
