#include "core/sleep_timer.h"
#include "core/store.h"

namespace core { namespace sleep_timer {
namespace {
bool s_loaded = false;
uint32_t s_seconds = kDefaultSeconds;

bool valid(uint32_t value) {
    return value >= kMinSeconds && value <= kMaxSeconds;
}
}

bool enabled() {
    return store::getBool("settings", "sleep_en", true);
}

bool setEnabled(bool enabled) {
    return store::putBool("settings", "sleep_en", enabled);
}

uint32_t seconds() {
    if (!s_loaded) {
        const uint32_t saved = store::getU32("settings", "sleep_sec", kDefaultSeconds);
        s_seconds = valid(saved) ? saved : kDefaultSeconds;
        s_loaded = true;
    }
    return s_seconds;
}

uint32_t milliseconds() {
    return seconds() * 1000u;
}

bool save(uint32_t value) {
    if (!valid(value) || !store::putU32("settings", "sleep_sec", value) ||
        store::getU32("settings", "sleep_sec", 0u) != value) return false;
    s_seconds = value;
    s_loaded = true;
    return true;
}

}}  // namespace core::sleep_timer
