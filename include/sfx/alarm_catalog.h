#pragma once
#include <stdint.h>
struct SfxDef;
namespace sfx { namespace alarms {
constexpr uint8_t kCount = 4;
struct Preset {
    const char* name;
    const SfxDef& preview;
    const SfxDef& ring;
    uint16_t repeatGapMs;
};
// Stable persisted IDs: Classic, Chime, Digital, Gentle. Invalid IDs use Classic.
// Callers with another invalid-input policy must normalize or reject first.
const Preset& preset(uint8_t id);
// Alarm/Timer editing historically ignores invalid IDs instead of playing a fallback.
void preview(uint8_t id);
} }
