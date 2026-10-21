/**
 * @file api.cpp
 * @brief Registry mechanics + assembly of the global `kami` table.
 *
 * This file owns no drawing logic — it assembles the kami.* API from the
 * registered namespaces (see api_registry.h). Each capability lives in its
 * own api_<name>.cpp exposing a Namespace accessor; the built-in list wires
 * them in one place (explicit accessors keep every namespace TU linked, and
 * the order deterministic). Adding a capability = new file + one line here.
 */

#include "widgets/api.h"
#include "widgets/api_registry.h"

#include <Arduino.h>
#include <string.h>

namespace widgets {

// Namespace accessors implemented in their own TUs.
const api::Namespace &transitNamespace();
const api::Namespace &drawNamespace(); // api_draw.cpp
const api::Namespace &sysNamespace();  // api_sys.cpp
const api::Namespace &deviceNamespace();
const api::Namespace &timerNamespace();
const api::Namespace &capabilitiesNamespace();
const api::Namespace &soundNamespace(); // api_sound.cpp

namespace {

// ── Root namespace ("") — installed directly onto the kami table ────────────
// kami.log stays on the root for now; it moves to kami.sys.log in P3.
int l_log(lua_State *L) {
    checkKamiExecution(L);
    // One bounded write, including for base print(). No unbounded stdout path.
    char text[257];
    size_t used = 0;
    const int arguments = lua_gettop(L);
    for (int i = 1; i <= arguments && used < sizeof(text) - 1; ++i) {
        size_t length = 0;
        const char *value = luaL_tolstring(L, i, &length);
        if (i > 1 && used < sizeof(text) - 1)
            text[used++] = '\t';
        const size_t remaining = sizeof(text) - 1 - used;
        if (length > remaining)
            length = remaining;
        memcpy(text + used, value, length);
        used += length;
        lua_pop(L, 1);
    }
    text[used] = 0;
    Serial.printf("[widget] %s\n", text);
    return 0;
}
const luaL_Reg kRootFns[] = {
    {"log", l_log},
    {nullptr, nullptr},
};
const api::Namespace kRootNamespace{"", kRootFns, nullptr, nullptr};

} // namespace

namespace api {

namespace {

// Fixed built-ins, enumerated once: no capacity limit or silently dropped entry.
const Namespace *const *builtins(size_t &count) {
    static const Namespace *const list[] = {
        &kRootNamespace,    &drawNamespace(),  &sysNamespace(),         &soundNamespace(),
        &deviceNamespace(), &timerNamespace(), &capabilitiesNamespace(), &transitNamespace()};
    count = sizeof(list) / sizeof(list[0]);
    return list;
}

} // namespace

void installAll(lua_State *L) {
    size_t count;
    const auto *list = builtins(count);

    lua_newtable(L); // kami = {}
    lua_pushinteger(L, 280);
    lua_setfield(L, -2, "W");
    lua_pushinteger(L, 240);
    lua_setfield(L, -2, "H");

    for (size_t i = 0; i < count; i++) {
        const Namespace &ns = *list[i];
        if (!ns.name || ns.name[0] == '\0') {
            luaL_setfuncs(L, ns.fns, 0); // install onto kami root
        } else {
            lua_newtable(L); // sub = {}
            luaL_setfuncs(L, ns.fns, 0);
            lua_setfield(L, -2, ns.name); // kami[name] = sub
        }
    }
    lua_setglobal(L, "kami"); // publish before onOpen hooks
    lua_pushcfunction(L, l_log);
    lua_setglobal(L, "print");

    for (size_t i = 0; i < count; i++) {
        if (list[i]->onOpen)
            list[i]->onOpen(L);
    }
}

void faultAll(lua_State *L) {
    size_t count;
    const auto *list = builtins(count);
    for (size_t i = count; i-- > 0;)
        if (list[i]->onFault) list[i]->onFault(L);
}

void closeAll(lua_State *L) {
    size_t count;
    const auto *list = builtins(count);
    for (size_t i = count; i-- > 0;) {
        if (list[i]->onClose)
            list[i]->onClose(L);
    }
}

} // namespace api

// ── Legacy entry points kept for LuaVm (api.h) ──────────────────────────────

void registerKamiApi(lua_State *L) {
    api::installAll(L);
}

void setKamiSurfaceSize(lua_State *L, int width, int height) {
    lua_getglobal(L, "kami");
    if (lua_istable(L, -1)) {
        lua_pushinteger(L, width);
        lua_setfield(L, -2, "W");
        lua_pushinteger(L, height);
        lua_setfield(L, -2, "H");
    }
    lua_pop(L, 1);
}

} // namespace widgets
