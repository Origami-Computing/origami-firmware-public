/**
 * @file api_sound.cpp
 * @brief The kami.sound.* namespace — fire-and-forget audio for widgets.
 *
 * A FOREGROUND widget can play a short sound: kami.sound.play("chime"). This
 * is the §12 Phase-1 "output" capability — no async, no arbitration beyond
 * what sfx::play already does (it silently no-ops if the audio bus is busy).
 *
 * IMPORTANT scope: widgets only execute while they are the active app, so this
 * alarms you when you are LOOKING at the widget (a countdown hitting 00:00).
 * It does NOT wake the device or fire when the widget is closed — a true
 * background alarm needs the timer/automation + wake-from-sleep system (Phase 4).
 */

#include "widgets/api.h"
#include "widgets/api_registry.h"

#include <string.h>

#include "sfx/sfx.h"
#include "sfx/sfx_lib.h"

namespace widgets {

namespace {

// Curated, named sounds — widgets pick an intent, not a raw waveform. Maps to
// the existing sfx bank so widgets can't synthesize arbitrary/annoying audio.
struct NamedSound { const char* name; const SfxDef* def; };
const NamedSound kSounds[] = {
    { "chime",   &sfx::kSfxAlarmChimeRing },
    { "alarm",   &sfx::kSfxAlarmDigitalRing },
    { "alert",   &sfx::kSfxAlarmClassicRing },
    { "success", &sfx::kSfxAlarmSave },
    { "error",   &sfx::kSfxAlarmError },
    { "tick",    &sfx::kSfxMenuTick },
    { "open",    &sfx::kSfxOpen },
};

// kami.sound.play(name) -> plays a named sound; returns true if recognized.
// Unknown name is a no-op returning false (never faults).
int l_play(lua_State* L) {
    checkKamiExecution(L);
    const char* name = luaL_checkstring(L, 1);
    for (const auto& s : kSounds) {
        if (strcmp(s.name, name) == 0) {
            sfx::play(*s.def);
            lua_pushboolean(L, 1);
            return 1;
        }
    }
    lua_pushboolean(L, 0);
    return 1;
}

const luaL_Reg kSoundFns[] = {
    { "play", l_play },
    { nullptr, nullptr },
};

}  // namespace

// Wired into the registry by api.cpp (explicit accessor → linker-safe).
const api::Namespace& soundNamespace() {
    static const api::Namespace ns{ "sound", kSoundFns, nullptr, nullptr };
    return ns;
}

}  // namespace widgets
