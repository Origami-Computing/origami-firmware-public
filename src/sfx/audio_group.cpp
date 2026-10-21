#include "sfx/audio_group.h"

namespace sfx {
namespace { AudioGroup* s_activeGroup = nullptr; }

void AudioGroup::add(sfx::SoundPlayer* node) {
    if (!node || count >= kMaxSounds) return;
    nodes[count++] = node;
}

void AudioGroup::clear() {
    for (uint8_t i = 0; i < count; i++) {
        if (nodes[i]) nodes[i]->stop();
    }
    count = 0;
}

void AudioGroup::service(uint32_t now) {
    for (uint8_t i = 0; i < count; i++) {
        if (nodes[i]) nodes[i]->service(now);
    }
}

void setActiveAudioGroup(AudioGroup* group) { s_activeGroup = group; }

AudioGroup* activeAudioGroup() { return s_activeGroup; }

} // namespace sfx
