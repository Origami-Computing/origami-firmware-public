#include "widgets/probe.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

extern "C" {
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
}

namespace widgets {

namespace {

// ── PSRAM-backed allocator ───────────────────────────────────────────────────
// The real widget runtime will confine Lua to a fixed PSRAM arena with a hard
// ceiling. For the spike we route every allocation to SPIRAM and tally the live
// byte count so we can report Lua's true heap footprint for a trivial script.

struct AllocStats {
    size_t live = 0;
    size_t peak = 0;
};

void* psramAlloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    auto* st = static_cast<AllocStats*>(ud);
    if (nsize == 0) {
        if (ptr) { heap_caps_free(ptr); st->live -= osize; }
        return nullptr;
    }
    void* np = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM);
    if (!np) return nullptr;  // OOM — Lua raises LUA_ERRMEM, never crashes the device
    st->live += nsize - (ptr ? osize : 0);
    if (st->live > st->peak) st->peak = st->live;
    return np;
}

// ── Sandboxed library set ────────────────────────────────────────────────────
// Exactly the libs a widget may touch: no io, os, package, or debug.
void openSafeLibs(lua_State* L) {
    static const luaL_Reg kLibs[] = {
        { LUA_GNAME,       luaopen_base },
        { LUA_TABLIBNAME,  luaopen_table },
        { LUA_STRLIBNAME,  luaopen_string },
        { LUA_MATHLIBNAME, luaopen_math },
        { LUA_UTF8LIBNAME, luaopen_utf8 },
        { LUA_COLIBNAME,   luaopen_coroutine },
    };
    for (const auto& lib : kLibs) {
        luaL_requiref(L, lib.name, lib.func, 1);
        lua_pop(L, 1);
    }
}

// Instruction-count watchdog — proves we can bound runaway scripts. Trips an
// error if a single call burns more than the budget of VM instructions.
constexpr int kInstructionBudget = 200000;
void countHook(lua_State* L, lua_Debug*) {
    luaL_error(L, "widget exceeded instruction budget");
}

}  // namespace

void probeRun() {
    Serial.println("[lua-probe] ── Phase-0 Lua 5.4 spike ──");
    Serial.printf("[lua-probe] %s\n", LUA_RELEASE);
    Serial.printf("[lua-probe] LUA_INTEGER=%u bytes  LUA_NUMBER=%u bytes\n",
                  (unsigned)sizeof(LUA_INTEGER), (unsigned)sizeof(LUA_NUMBER));

    const size_t psramBefore = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    AllocStats stats;
    lua_State* L = lua_newstate(psramAlloc, &stats);
    if (!L) {
        Serial.println("[lua-probe] FAILED: lua_newstate returned null (PSRAM OOM?)");
        return;
    }
    const size_t afterState = stats.live;

    openSafeLibs(L);
    const size_t afterLibs = stats.live;

    lua_sethook(L, countHook, LUA_MASKCOUNT, kInstructionBudget);

    // A widget-shaped script: state, a loop, string + math work, returns a value.
    static const char* kScript =
        "local sum = 0\n"
        "for i = 1, 1000 do sum = sum + i * 2 end\n"
        "local s = string.format('hello %d / sqrt=%.2f', sum, math.sqrt(sum))\n"
        "return s\n";

    const uint32_t t0 = micros();
    const int rc = luaL_dostring(L, kScript);
    const uint32_t elapsedUs = micros() - t0;

    if (rc != LUA_OK) {
        Serial.printf("[lua-probe] script error: %s\n", lua_tostring(L, -1));
    } else {
        Serial.printf("[lua-probe] script result: \"%s\"\n", lua_tostring(L, -1));
    }

    const size_t peak = stats.peak;
    lua_close(L);

    const size_t psramAfter = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

    Serial.println("[lua-probe] ── results ──");
    Serial.printf("[lua-probe] VM state alloc:      %u bytes\n", (unsigned)afterState);
    Serial.printf("[lua-probe] + safe stdlib:       %u bytes (total live)\n", (unsigned)afterLibs);
    Serial.printf("[lua-probe] peak heap (PSRAM):   %u bytes\n", (unsigned)peak);
    Serial.printf("[lua-probe] exec time:           %u us\n", (unsigned)elapsedUs);
    Serial.printf("[lua-probe] PSRAM reclaimed:     %d bytes (leak check, want ~0)\n",
                  (int)((long)psramAfter - (long)psramBefore));
    Serial.println("[lua-probe] ── done ── (flash cost is in the build size report)");
}

}  // namespace widgets
