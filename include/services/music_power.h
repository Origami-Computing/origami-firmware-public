#pragma once
#include <stdint.h>
#include "services/background_network.h"

namespace services { namespace audio {
// UI-task owner of Music's persisted preference, backlight and paired radio
// handoff. Audio/I2S keep running. Call leave before another view takes over.
class MusicPower {
public:
    void load();
    bool enabled() const { return enabled_; }
    bool setEnabled(bool enabled);
    void activity(uint32_t now);
    // Only eligible for visible, local playback. Network acquisition is bounded
    // (50 ms per worker); a failed handoff leaves Wi-Fi alone and retries later.
    void tick(uint32_t now, bool eligible);
    void leave();
    bool dark() const { return dark_; }
    bool radioOff() const { return radioOff_; }
private:
    background_network::RadioPause network_;
    bool enabled_ = false, dark_ = false, radioOff_ = false;
    bool attempted_ = false;
    uint32_t activityMs_ = 0, attemptMs_ = 0;
};
} }
