#pragma once
#include "services/vibration.h"
#include <stdint.h>
#include "sfx/sound_player.h"

namespace services { namespace alerts {

// UI-task repeating sound/motor session. Caller decides when its workflow is
// audible; the existing vibration owner check protects a preempting alarm.
class IntervalFeedback {
public:
    explicit IntervalFeedback(services::vibration::Owner owner) : owner_(owner) {}
    IntervalFeedback(const IntervalFeedback&) = delete;
    IntervalFeedback& operator=(const IntervalFeedback&) = delete;
    void start(uint8_t sound, uint8_t vibration, uint32_t now);
    void poll(uint32_t now);
    void stop();
    bool active() const { return active_; }
    bool repeating() const { return sound_.repeating; }
    static void previewSound(uint8_t sound);
private:
    services::vibration::Owner owner_;
    sfx::SoundPlayer sound_;
    uint32_t started_ = 0;
    uint8_t vibration_ = 0;
    bool active_ = false;
};
} }
