/**
 * @file api_sys.cpp
 * @brief The kami.sys.* namespace — read-only system state for widgets.
 *
 * First member: kami.sys.time() — the device wall clock. Widgets get uptime ms
 * via the `now` arg to tick/draw, but real local time needs the RTC, which the
 * sandbox can't reach directly (no os library). This exposes it as a plain
 * table, always safe to call: when the clock hasn't SNTP-synced yet it returns
 * { synced = false } and nothing else, so a widget can show "--:--" until then.
 */

#include "widgets/api.h"
#include "widgets/api_registry.h"

#include <time.h>

#include "core/board_time.h"

namespace widgets {

namespace {

// kami.sys.time() -> table:
//   { synced = false }                                   (clock not yet trusted)
//   { synced = true, hour, min, sec, day, month, year,
//     wday (0=Sun), epoch, hhmm = "HH:MM" }              (local wall clock)
int l_time(lua_State* L) {
    lua_newtable(L);
    const bool synced = core::time::isSynced() && core::time::timezoneConfigured();
    lua_pushboolean(L, synced ? 1 : 0);
    lua_setfield(L, -2, "synced");
    if (!synced) return 1;

    const time_t t = time(nullptr);
    struct tm lt;
    localtime_r(&t, &lt);

    auto seti = [&](const char* k, lua_Integer v) {
        lua_pushinteger(L, v);
        lua_setfield(L, -2, k);
    };
    seti("hour",  lt.tm_hour);
    seti("min",   lt.tm_min);
    seti("sec",   lt.tm_sec);
    seti("day",   lt.tm_mday);
    seti("month", lt.tm_mon + 1);
    seti("year",  lt.tm_year + 1900);
    seti("wday",  lt.tm_wday);            // 0 = Sunday
    seti("epoch", (lua_Integer)t);

    char hhmm[8] = {};
    core::time::formatClockHHMM(hhmm, sizeof(hhmm));
    lua_pushstring(L, hhmm);
    lua_setfield(L, -2, "hhmm");
    return 1;
}

const luaL_Reg kSysFns[] = {
    { "time", l_time },
    { nullptr, nullptr },
};

}  // namespace

// Wired into the registry by api.cpp (explicit accessor → linker-safe).
const api::Namespace& sysNamespace() {
    static const api::Namespace ns{ "sys", kSysFns, nullptr, nullptr };
    return ns;
}

}  // namespace widgets
