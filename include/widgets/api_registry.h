#pragma once

/**
 * @file api_registry.h
 * @brief Modular registry for the kami.* API namespaces (spec §5.1).
 *
 * Each capability is a self-contained Namespace — a group of Lua functions
 * installed under one sub-table of the global `kami` (or onto the kami root
 * when name is ""). api.cpp assembles every registered namespace onto `kami`
 * at VM open via installAll(). Adding a capability is a new api_<name>.cpp
 * file exposing a Namespace accessor + one entry in api.cpp's built-in list —
 * api.cpp never needs to know the individual functions.
 */

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

namespace widgets {
namespace api {

/** One kami.* capability: a named group of Lua C functions plus optional
 *  per-VM lifecycle hooks. */
struct Namespace {
    const char*     name;                  ///< "draw", "sys", …; "" installs on the kami root.
    const luaL_Reg* fns;                   ///< null-terminated function list.
    void          (*onOpen)(lua_State*);   ///< optional: run after the table is installed.
    void          (*onClose)(lua_State*);  ///< optional: native, non-throwing, allocation-free teardown; tolerate partial open.
    void          (*onFault)(lua_State*); ///< optional: same teardown constraints; repeated calls allowed.
};

/** Build the global `kami` table on L: W/H constants + every registered
 *  namespace, then run each namespace's onOpen hook. Call once at VM open. */
void installAll(lua_State* L);

/** Run every namespace's onClose hook. Call once at VM close, before lua_close. */
void closeAll(lua_State* L);
/** Release active operations when a VM faults but its error view remains mounted. */
void faultAll(lua_State* L);

}  // namespace api
}  // namespace widgets
