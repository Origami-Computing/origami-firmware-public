#include "sfx/alarm_catalog.h"
#include "sfx/sfx_lib.h"

namespace sfx { namespace alarms {
namespace {
const Preset kPresets[kCount] = {
    {"Daybreak", kSfxAlarmClassicPreview, kSfxAlarmClassicRing, 1050},
    {"Sunrise", kSfxAlarmChimePreview, kSfxAlarmChimeRing, 1150},
    {"Morning Steps", kSfxAlarmDigitalPreview, kSfxAlarmDigitalRing, 900},
    {"Gentle Bells", kSfxAlarmGentlePreview, kSfxAlarmGentleRing, 1300},
};
}
const Preset& preset(uint8_t id) { return kPresets[id < kCount ? id : 0]; }
void preview(uint8_t id) { if (id < kCount) sfx::play(kPresets[id].preview); }
} }
