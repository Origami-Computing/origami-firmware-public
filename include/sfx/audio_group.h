#pragma once
#include "sfx/sound_player.h"

namespace sfx {

struct AudioGroup {
    static constexpr uint8_t kMaxSounds = 8;

    sfx::SoundPlayer* nodes[kMaxSounds] = {};
    uint8_t    count             = 0;

    void add(sfx::SoundPlayer* node);
    void clear();
    void service(uint32_t now);
};

// Attaching/detaching does not stop players. clear() stops registered members.
void setActiveAudioGroup(AudioGroup* group);
AudioGroup* activeAudioGroup();
} // namespace sfx
