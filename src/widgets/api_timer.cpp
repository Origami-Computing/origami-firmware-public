#include "control/device_api.h"
#include "widgets/api.h"
#include "widgets/api_registry.h"
#include <stdio.h>
#include <string.h>

namespace widgets {
namespace {
using Operation = control::Operation;
int failure(lua_State *L, const char *code, bool mayHaveApplied = false) {
    lua_pushnil(L);
    lua_newtable(L);
    lua_pushstring(L, code);
    lua_setfield(L, -2, "code");
    if (mayHaveApplied) {
        lua_pushboolean(L, 1);
        lua_setfield(L, -2, "may_have_applied");
    }
    return 2;
}
void integer(lua_State *L, const char *key, uint32_t value) {
    lua_pushinteger(L, (lua_Integer)value);
    lua_setfield(L, -2, key);
}
bool readRevision(const char *text, size_t length, uint32_t &value) {
    if (!text || !length)
        return false;
    value = 0;
    for (const char *p = text; p != text + length; ++p) {
        if (*p < '0' || *p > '9')
            return false;
        const unsigned digit = *p - '0';
        if (value > (UINT32_MAX - digit) / 10u)
            return false;
        value = value * 10u + digit;
    }
    return true;
}
int timerCall(lua_State *L) {
    checkKamiExecution(L);
    const auto operation = (Operation)lua_tointeger(L, lua_upvalueindex(1));
    const bool mutates = operation != Operation::TimerGet;
    if (mutates && isKamiDrawing(L))
        return failure(L, "draw_phase");
    if (mutates && !isKamiCallback(L))
        return failure(L, "callback_required");
    control::Request request;
    request.operation = operation;
    const int arguments = lua_gettop(L);
    if (arguments > 1)
        return luaL_error(L, "timer expects at most one options table");
    if (!mutates && arguments)
        return luaL_error(L, "timer.get expects no arguments");
    if (arguments) {
        luaL_checktype(L, 1, LUA_TTABLE);
        lua_pushnil(L);
        while (lua_next(L, 1)) {
            if (lua_type(L, -2) != LUA_TSTRING)
                return luaL_error(L, "timer option keys must be strings");
            size_t keyLength;
            const char *key = lua_tolstring(L, -2, &keyLength);
            if (strlen(key) != keyLength)
                return luaL_error(L, "timer option keys cannot contain NUL");
            if (!strcmp(key, "duration_seconds") && operation == Operation::TimerStart) {
                if (!lua_isinteger(L, -1))
                    return luaL_error(L, "duration_seconds must be an integer");
                const lua_Integer seconds = lua_tointeger(L, -1);
                if (seconds < 0 || (uint64_t)seconds > UINT32_MAX)
                    return failure(L, "invalid_argument");
                request.durationSeconds = (uint32_t)seconds;
            } else if (!strcmp(key, "expected_revision")) {
                if (lua_type(L, -1) != LUA_TSTRING)
                    return luaL_error(L, "expected_revision must be a decimal string");
                size_t length;
                const char *revision = lua_tolstring(L, -1, &length);
                if (lua_type(L, -1) != LUA_TSTRING ||
                    !readRevision(revision, length, request.expectedRevision))
                    return luaL_error(
                        L, "expected_revision must be an unsigned 32-bit decimal string");
                request.checkRevision = true;
            } else
                return luaL_error(L, "unknown timer option '%s'", key);
            lua_pop(L, 1);
        }
    }
    if (operation == Operation::TimerStart && !request.durationSeconds)
        return failure(L, "invalid_argument");
    const auto result = control::execute(request);
    if (result.error != control::Error::None)
        return failure(L, control::errorName(result.error),
                       result.error == control::Error::StorageError);
    lua_newtable(L);
    static const char *states[] = {"idle", "running", "paused", "done"};
    lua_pushstring(L, states[(unsigned)result.timer.state]);
    lua_setfield(L, -2, "state");
    integer(L, "duration_seconds", result.timer.duration);
    integer(L, "remaining_seconds", result.timer.remaining);
    char revision[11];
    snprintf(revision, sizeof(revision), "%lu", (unsigned long)result.timerRevision);
    lua_pushstring(L, revision);
    lua_setfield(L, -2, "revision");
    return 1;
}
void install(lua_State *L) {
    static const struct {
        const char *name;
        Operation operation;
    } methods[] = {{"get", Operation::TimerGet},
                   {"start", Operation::TimerStart},
                   {"pause", Operation::TimerPause},
                   {"resume", Operation::TimerResume},
                   {"cancel", Operation::TimerCancel}};
    lua_getglobal(L, "kami");
    lua_getfield(L, -1, "timer");
    for (const auto &method : methods) {
        lua_pushinteger(L, (lua_Integer)method.operation);
        lua_pushcclosure(L, timerCall, 1);
        lua_setfield(L, -2, method.name);
    }
    lua_pop(L, 2);
}
const luaL_Reg kFunctions[] = {{nullptr, nullptr}};
} // namespace
const api::Namespace &timerNamespace() {
    static const api::Namespace ns{"timer", kFunctions, install, nullptr};
    return ns;
}
} // namespace widgets
