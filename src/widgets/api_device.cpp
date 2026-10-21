#include "control/device_api.h"
#include "widgets/api.h"
#include "widgets/api_registry.h"
#include <stdio.h>

namespace widgets {
namespace {
void integer(lua_State *L, const char *key, lua_Integer value) {
    lua_pushinteger(L, value);
    lua_setfield(L, -2, key);
}
void text(lua_State *L, const char *key, const char *value) {
    lua_pushstring(L, value);
    lua_setfield(L, -2, key);
}
int snapshot(lua_State *L) {
    checkKamiExecution(L);
    if (lua_gettop(L))
        return luaL_error(L, "snapshot expects no arguments");
    const auto result = control::execute({});
    lua_newtable(L);
    text(L, "app", result.app);
    integer(L, "sfx_volume", result.volume);
    lua_newtable(L);
    static const char *states[] = {"idle", "running", "paused", "done"};
    text(L, "state", states[(unsigned)result.timer.state]);
    integer(L, "duration_seconds", result.timer.duration);
    integer(L, "remaining_seconds", result.timer.remaining);
    // Firmware Lua uses 32-bit signed integers. Revisions retain all 32 bits.
    char revision[11];
    snprintf(revision, sizeof(revision), "%lu", (unsigned long)result.timerRevision);
    text(L, "revision", revision);
    lua_setfield(L, -2, "timer");
    return 1;
}
const luaL_Reg kFunctions[] = {{"snapshot", snapshot}, {nullptr, nullptr}};
} // namespace
const api::Namespace &deviceNamespace() {
    static const api::Namespace ns{"device", kFunctions, nullptr, nullptr};
    return ns;
}
} // namespace widgets
