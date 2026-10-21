#include "services/vibration.h"
#include "services/interval_feedback.h"
#include "sfx/sfx.h"
#include "sfx/alarm_catalog.h"

namespace services { namespace alerts {
void IntervalFeedback::start(uint8_t sound, uint8_t vibration, uint32_t now) {
    if (active_) stop();
    sfx::initSoundPlayer(sound_);
    const auto& preset = sfx::alarms::preset(sound < sfx::alarms::kCount ? sound : 3);
    sound_.repeatFromDef(preset.ring,preset.repeatGapMs,now);
    started_ = now;
    vibration_ = vibration;
    active_ = true;
}
void IntervalFeedback::poll(uint32_t now) {
    if (!active_) return;
    services::vibration::drive(owner_,vibration_,now-started_);
    sound_.service(now);
}
void IntervalFeedback::stop() {
    sound_.stop();
    services::vibration::release(owner_);
    active_ = false;
}
void IntervalFeedback::previewSound(uint8_t sound) { sfx::play(sfx::alarms::preset(sound < sfx::alarms::kCount ? sound : 3).preview); }
} }
