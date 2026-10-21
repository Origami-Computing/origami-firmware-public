#include "services/music_power.h"
#include "core/store.h"
#include "hal/hal_display.h"
#include "hal/hal_wifi.h"

namespace services { namespace audio {
void MusicPower::load() { leave(); enabled_ = core::store::getBool("music", "low_power", false); }
bool MusicPower::setEnabled(bool enabled) {
    if (!core::store::putBool("music", "low_power", enabled)) return false;
    enabled_ = enabled;
    if (!enabled_) leave();
    return true;
}
void MusicPower::activity(uint32_t now) {
    activityMs_ = now;
    if (dark_) { hal::display::setBacklight(true); dark_ = false; }
}
void MusicPower::leave() {
    if (dark_) { hal::display::setBacklight(true); dark_ = false; }
    if (radioOff_) {
        // The Core-0 Wi-Fi manager owns driver restart. Queue it before
        // reopening admission; consumers check suspended/connectivity while
        // restoration and association complete.
        hal::wifi::scheduleRestore();
        radioOff_ = false;
    }
    network_.release();
    attempted_ = false;
}
void MusicPower::tick(uint32_t now, bool eligible) {
    if (!enabled_ || !eligible) { leave(); activityMs_ = now; return; }
    if (!dark_ && uint32_t(now - activityMs_) >= 15000) {
        hal::display::setBacklight(false);
        dark_ = true;
    }
    if (radioOff_ || (attempted_ && uint32_t(now - attemptMs_) < 10000)) return;
    attempted_ = true;
    attemptMs_ = now;
    // Never take over a portal, a transition, or someone else's suspension.
    if (agent_coordination::isSessionActive() ||
        !hal::wifi::taskBooted() || hal::wifi::apStarted() ||
        hal::wifi::radioState() != hal::wifi::RadioState::WifiOwned) return;
    if (network_.acquire(50) != background_network::RadioPause::Result::Acquired) return;
    hal::wifi::stop();
    radioOff_ = true;
}
} }
