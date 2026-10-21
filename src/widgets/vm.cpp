#include "widgets/vm.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <string.h>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
}

#include "ui/scene.h"
#include "widgets/api.h"
#include "widgets/api_registry.h"

namespace widgets {

namespace {

// Per-call instruction budget. Each protected call re-arms the count hook to
// this value, so one tick/draw/input may execute at most this many VM
// instructions before being aborted with a catchable error.
constexpr int kInstructionBudget = 150000;

constexpr int kHookQuantum = 1000;

int loadTextOnly(lua_State *L) {
    const int arguments = lua_gettop(L) > 3 ? lua_gettop(L) : 3;
    lua_settop(L, arguments);
    lua_pushliteral(L, "t");
    lua_replace(L, 3);
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, arguments, LUA_MULTRET);
    return lua_gettop(L);
}

// Opened inside a protected call so an OOM during library init can't panic.
int pInitLibs(lua_State *L) {
    static const luaL_Reg kSafeLibs[] = {
        {LUA_GNAME, luaopen_base},        {LUA_TABLIBNAME, luaopen_table},
        {LUA_STRLIBNAME, luaopen_string}, {LUA_MATHLIBNAME, luaopen_math},
        {LUA_UTF8LIBNAME, luaopen_utf8},  {LUA_COLIBNAME, luaopen_coroutine},
    };
    for (const auto &lib : kSafeLibs) {
        luaL_requiref(L, lib.name, lib.func, 1);
        lua_pop(L, 1);
    }
    // Base Lua exposes file loaders even when io/package are absent.
    lua_pushnil(L);
    lua_setglobal(L, "dofile");
    lua_pushnil(L);
    lua_setglobal(L, "loadfile");
    lua_getglobal(L, "load");
    lua_pushcclosure(L, loadTextOnly, 1);
    lua_setglobal(L, "load");
    registerKamiApi(L);
    // kami.data always exists so scripts can index it unconditionally;
    // setDataJson replaces it when the widget ships agent-supplied data.
    lua_getglobal(L, "kami");
    lua_newtable(L);
    lua_setfield(L, -2, "data");
    lua_pop(L, 1);
    return 0;
}

struct ScriptChunk {
    const char *source;
    size_t length;
    const char *name;
};
int pLoadScript(lua_State *L) {
    const auto *chunk = static_cast<const ScriptChunk *>(lua_touserdata(L, 1));
    lua_newtable(L);
    lua_setglobal(L, "widget");
    if (luaL_loadbufferx(L, chunk->source, chunk->length, chunk->name, "t") != LUA_OK)
        return lua_error(L);
    lua_call(L, 0, 0);
    return 0;
}

// Recursively push a JSON value as the equivalent Lua value. Arrays become
// 1-based tables. Depth-capped — agent data is shallow by construction.
void pushJsonValue(lua_State *L, JsonVariantConst v, int depth) {
    if (depth > 8) {
        lua_pushnil(L);
        return;
    }
    if (v.is<JsonObjectConst>()) {
        lua_newtable(L);
        for (JsonPairConst kv : v.as<JsonObjectConst>()) {
            pushJsonValue(L, kv.value(), depth + 1);
            lua_setfield(L, -2, kv.key().c_str());
        }
    } else if (v.is<JsonArrayConst>()) {
        lua_newtable(L);
        int i = 1;
        for (JsonVariantConst item : v.as<JsonArrayConst>()) {
            pushJsonValue(L, item, depth + 1);
            lua_rawseti(L, -2, i++);
        }
    } else if (v.is<const char *>()) {
        lua_pushstring(L, v.as<const char *>());
    } else if (v.is<bool>()) {
        lua_pushboolean(L, v.as<bool>() ? 1 : 0);
    } else if (v.is<long long>()) {
        lua_pushinteger(L, (lua_Integer)v.as<long long>());
    } else if (v.is<double>()) {
        lua_pushnumber(L, (lua_Number)v.as<double>());
    } else {
        lua_pushnil(L);
    }
}

// Runs protected so a Lua OOM while building the data table faults cleanly
// instead of panicking. Arg 1 is the parsed JsonDocument (light userdata).
int pSetData(lua_State *L) {
    auto *doc = static_cast<JsonDocument *>(lua_touserdata(L, 1));
    lua_getglobal(L, "kami");
    pushJsonValue(L, doc->as<JsonVariantConst>(), 0);
    lua_setfield(L, -2, "data");
    return 0;
}

} // namespace

void LuaVm::armBudget(uint32_t elapsedLimitMs) {
    budgetStartedMs_ = millis();
    elapsedLimitMs_ = elapsedLimitMs;
    instructionsRemaining_ = kInstructionBudget;
    budgetExceeded_ = false;
    lua_sethook(L_, countHook, LUA_MASKCOUNT, kHookQuantum);
}

void LuaVm::countHook(lua_State *state, lua_Debug *) {
    void *owner = nullptr;
    lua_getallocf(state, &owner); // All coroutines share the VM allocator owner.
    auto *self = static_cast<LuaVm *>(owner);
    self->instructionsRemaining_ -= kHookQuantum;
    if (self->instructionsRemaining_ <= 0)
        self->budgetExceeded_ = true;
    checkKamiExecution(state);
}

bool isKamiCallback(lua_State *state) {
    void *owner = nullptr;
    lua_getallocf(state, &owner);
    return static_cast<LuaVm *>(owner)->inCallback_;
}

void checkKamiExecution(lua_State *state) {
    void *owner = nullptr;
    lua_getallocf(state, &owner);
    auto *self = static_cast<LuaVm *>(owner);
    if (!self->budgetExceeded_ &&
        uint32_t(millis() - self->budgetStartedMs_) < self->elapsedLimitMs_)
        return;
    self->budgetExceeded_ = true;
    // Catching the error must not grant another budget, including in a coroutine.
    lua_sethook(state, LuaVm::countHook, LUA_MASKCOUNT, 1);
    luaL_error(state, "widget exceeded instruction budget or callback deadline");
}

void *LuaVm::allocHook(void *ud, void *ptr, size_t osize, size_t nsize) {
    auto *self = static_cast<LuaVm *>(ud);
    if (nsize == 0) {
        if (ptr) {
            heap_caps_free(ptr);
            self->live_ -= osize;
        }
        return nullptr;
    }
    const size_t prev = ptr ? osize : 0;
    // Enforce the hard ceiling before allocating — over-limit returns null,
    // which Lua turns into a catchable LUA_ERRMEM rather than a device crash.
    if (self->ceiling_ && self->live_ - prev + nsize > self->ceiling_)
        return nullptr;
    void *np = heap_caps_realloc(ptr, nsize, MALLOC_CAP_SPIRAM);
    if (!np)
        return nullptr;
    self->live_ += nsize - prev;
    if (self->live_ > self->peak_)
        self->peak_ = self->live_;
    return np;
}

bool LuaVm::open(size_t memCeilingBytes) {
    if (L_)
        return true;
    live_ = peak_ = 0;
    ceiling_ = memCeilingBytes;
    faulted_ = false;
    errBuf_[0] = '\0';

    L_ = lua_newstate(allocHook, this);
    if (!L_) {
        setFault("VM alloc failed (PSRAM OOM)");
        return false;
    }

    armBudget(250);
    lua_pushcfunction(L_, pInitLibs);
    if (lua_pcall(L_, 0, 0, 0) != LUA_OK) {
        setFault(lua_tostring(L_, -1));
        close();
        return false;
    }
    return true;
}

void LuaVm::close() {
    if (L_) {
        api::closeAll(L_);
        lua_close(L_);
        L_ = nullptr;
    }
    live_ = peak_ = 0;
}

void LuaVm::setFault(const char *msg) {
    faulted_ = true;
    if (L_) api::faultAll(L_);
    snprintf(errBuf_, sizeof(errBuf_), "%s", msg ? msg : "unknown lua error");
    Serial.printf("[lua-vm] fault: %s\n", errBuf_);
}

bool LuaVm::loadScript(const char *src, size_t len, const char *chunkName) {
    if (!L_)
        return false;
    faulted_ = false;
    errBuf_[0] = '\0';

    const ScriptChunk chunk{src, len, chunkName};
    lua_pushcfunction(L_, pLoadScript);
    lua_pushlightuserdata(L_, const_cast<ScriptChunk *>(&chunk));
    armBudget(250);
    if (lua_pcall(L_, 1, 0, 0) != LUA_OK) {
        setFault(lua_tostring(L_, -1));
        lua_pop(L_, 1);
        return false;
    }
    if (budgetExceeded_) {
        setFault("widget exceeded instruction budget");
        return false;
    }
    return true;
}

bool LuaVm::setDataJson(const char *json, size_t len) {
    if (!L_ || !json || len == 0)
        return false;
    JsonDocument doc;
    if (deserializeJson(doc, json, len) || !doc.is<JsonObjectConst>()) {
        Serial.println("[lua-vm] setDataJson: invalid/non-object JSON, ignored");
        return false;
    }
    armBudget(250);
    lua_pushcfunction(L_, pSetData);
    lua_pushlightuserdata(L_, &doc);
    if (lua_pcall(L_, 1, 0, 0) != LUA_OK) {
        setFault(lua_tostring(L_, -1));
        lua_pop(L_, 1);
        return false;
    }
    return true;
}

struct LuaVm::Invocation {
    const char *name;
    uint32_t now;
    const char *input;
    int32_t delta;
    int16_t width, height;
    int arguments, results;
};

int LuaVm::invokeProtected(lua_State *state) {
    const auto *call = static_cast<const Invocation *>(lua_touserdata(state, 1));
    if (call->width > 0)
        setKamiSurfaceSize(state, call->width, call->height);
    lua_getglobal(state, "widget");
    if (!lua_istable(state, -1))
        return 0;
    lua_getfield(state, -1, call->name);
    if (!lua_isfunction(state, -1))
        return 0;
    if (call->arguments) {
        if (call->input) {
            lua_createtable(state, 0, 2);
            lua_pushstring(state, call->input);
            lua_setfield(state, -2, "type");
            lua_pushinteger(state, call->delta);
            lua_setfield(state, -2, "delta");
        } else
            lua_pushinteger(state, (lua_Integer)call->now);
    }
    lua_call(state, call->arguments, call->results);
    return call->results;
}

bool LuaVm::invoke(const Invocation &call) {
    if (!L_ || faulted_)
        return false;
    // Lookup, metamethods, argument tables and surface updates can all allocate
    // or run Lua. Keep them inside the same protected boundary as the callback.
    lua_pushcfunction(L_, invokeProtected);
    lua_pushlightuserdata(L_, const_cast<Invocation *>(&call));
    armBudget();
    inCallback_ = true;
    const int status = lua_pcall(L_, 1, call.results, 0);
    inCallback_ = false;
    if (status != LUA_OK) {
        setFault(lua_tostring(L_, -1));
        lua_pop(L_, 1);
        return false;
    }
    if (budgetExceeded_) {
        lua_pop(L_, call.results);
        setFault("widget exceeded instruction budget");
        return false;
    }
    return true;
}

bool LuaVm::callStart() {
    return invoke({"start", 0, nullptr, 0, 0, 0, 0, 0});
}
bool LuaVm::callTick(uint32_t now) {
    return invoke({"tick", now, nullptr, 0, 0, 0, 1, 0});
}
bool LuaVm::callInput(const char *type, int32_t delta) {
    return invoke({"input", 0, type, delta, 0, 0, 1, 0});
}
bool LuaVm::callChord(bool &handledOut) {
    handledOut = false;
    if (!invoke({"chord", 0, nullptr, 0, 0, 0, 0, 1}))
        return false;
    handledOut = lua_toboolean(L_, -1) != 0;
    lua_pop(L_, 1);
    return true;
}
void LuaVm::callStop() {
    (void)invoke({"stop", 0, nullptr, 0, 0, 0, 0, 0});
}

bool LuaVm::buildScene(ui::scene::Scene &out, uint32_t now, int16_t w, int16_t h) {
    if (!L_ || faulted_)
        return false;
    setKamiScene(L_, &out);
    bool ok = invoke({"draw", now, nullptr, 0, w, h, 1, 0});
    setKamiScene(L_, nullptr);
    // A script may catch a draw error with pcall; never render a truncated frame.
    if (ok && out.overflowed()) {
        setFault("widget scene capacity exceeded");
        ok = false;
    }
    return ok;
}

} // namespace widgets
