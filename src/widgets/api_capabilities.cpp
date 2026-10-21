#include "control/device_api.h"
#include "widgets/api.h"
#include "widgets/api_registry.h"
#include <string.h>

namespace widgets {
namespace {
void text(lua_State *L, const char *key, const char *value) {
    lua_pushstring(L, value);
    lua_setfield(L, -2, key);
}
void describe(lua_State *L, const control::Descriptor &item) {
    lua_newtable(L);
    text(L, "operation", item.name);
    text(L, "binding", item.luaBinding);
    text(L, "description", item.summary);
    lua_pushboolean(L, item.mutates);
    lua_setfield(L, -2, "mutates");
    if (item.argument) {
        lua_newtable(L);
        text(L, "name", item.argument);
        text(L, "type", item.type);
        lua_pushinteger(L, item.minimum);
        lua_setfield(L, -2, "minimum");
        lua_pushinteger(L, item.maximum);
        lua_setfield(L, -2, "maximum");
        lua_setfield(L, -2, "argument");
    }
}
int list(lua_State *L) {
    checkKamiExecution(L);
    if (lua_gettop(L))
        return luaL_error(L, "capabilities.list expects no arguments");
    lua_newtable(L);
    unsigned count;
    int index = 0;
    const auto *catalog = control::descriptors(count);
    for (unsigned i = 0; i < count; ++i)
        if (catalog[i].luaBinding) {
            describe(L, catalog[i]);
            lua_rawseti(L, -2, ++index);
        }
    return 1;
}
int describe(lua_State *L) {
    checkKamiExecution(L);
    if (lua_gettop(L) != 1 || lua_type(L, 1) != LUA_TSTRING)
        return luaL_error(L, "capabilities.describe expects one operation name");
    size_t length;
    const char *name = lua_tolstring(L, 1, &length);
    const auto *item = strlen(name) == length ? control::find(name) : nullptr;
    if (!item || !item->luaBinding) {
        lua_pushnil(L);
        return 1;
    }
    describe(L, *item);
    return 1;
}
const luaL_Reg kFunctions[] = {{"list", list}, {"describe", describe}, {nullptr, nullptr}};
} // namespace
const api::Namespace &capabilitiesNamespace() {
    static const api::Namespace ns{"capabilities", kFunctions, nullptr, nullptr};
    return ns;
}
} // namespace widgets
