#include "services/screen_brightness.h"
#include "core/store.h"
#include "hal/hal_display.h"

namespace services { namespace screen_brightness {
namespace {
bool initialized = false;
int value = kDefault, saved = kDefault;
int validated(int percent) { return percent < kMinimum ? kMinimum : percent > 100 ? 100 : percent; }
}
void initialize() {
  if (initialized) return;
  saved = value = validated(core::store::getU8("display", "brightness", kDefault));
  hal::display::setBrightness(value);
  initialized = true;
}
// Pure reads are also used by side-effect-free scene builders.
int percent() { return value; }
int savedPercent() { return saved; }
void set(int percent) {
  initialize();
  const int next = validated(percent);
  if (next == value) return;
  value = next;
  hal::display::setBrightness(value);
}
bool save() {
  initialize();
  if (value == saved) return true;
  if (!core::store::putU8("display", "brightness", value)) return false;
  saved = value;
  return true;
}
void discard() { initialize(); set(saved); }
} }
