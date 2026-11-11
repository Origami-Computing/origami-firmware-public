#pragma once
#include "widgets/api_registry.h"
#include <ArduinoJson.h>
namespace widgets { namespace json {
inline void push(lua_State *L, JsonVariantConst v, int depth) {
    if (depth > 8) {
        lua_pushnil(L);
        return;
    }
    if (v.is<JsonObjectConst>()) {
        lua_newtable(L);
        for (JsonPairConst kv : v.as<JsonObjectConst>()) {
            push(L, kv.value(), depth + 1);
            lua_setfield(L, -2, kv.key().c_str());
        }
    } else if (v.is<JsonArrayConst>()) {
        lua_newtable(L);
        int i = 1;
        for (JsonVariantConst item : v.as<JsonArrayConst>()) {
            push(L, item, depth + 1);
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

inline int protectedPush(lua_State* L){auto* doc=static_cast<JsonDocument*>(lua_touserdata(L,1));push(L,doc->as<JsonVariantConst>(),0);return 1;}
}}
