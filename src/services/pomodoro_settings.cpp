#include "services/pomodoro_settings.h"
#include "core/store.h"
#include "core/alert_vibration.h"

namespace services { namespace pomodoro {
void Settings::load() {
  auto minutes=[](uint8_t value) -> uint8_t { return value<1?1:value>99?99:value; };
  focusMinutes=minutes(core::store::getU8("pomodoro","work",25));
  breakMinutes=minutes(core::store::getU8("pomodoro","break",5));
  sound=core::store::getU8("pomodoro","sound",0)%4;
  vibration=core::alert_vibration::validId(core::store::getU8("pomodoro","vibration",0));
}
bool Settings::save() const {
  bool ok=core::store::putU8("pomodoro","work",focusMinutes);
  ok=core::store::putU8("pomodoro","break",breakMinutes)&&ok;
  ok=core::store::putU8("pomodoro","sound",sound)&&ok;
  return core::store::putU8("pomodoro","vibration",vibration)&&ok;
}
} }
