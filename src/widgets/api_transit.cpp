#include "widgets/api.h"
#include "widgets/sync.h"
#include "widgets/api_registry.h"
#include "services/muni_transit.h"
#include <cstring>
namespace widgets { namespace {
const char ownerKey=0;
uint32_t owner(lua_State* L){lua_rawgetp(L,LUA_REGISTRYINDEX,&ownerKey);auto n=uint32_t(lua_tointeger(L,-1));lua_pop(L,1);return n;}
void close(lua_State* L){services::muni::stop(owner(L));}
void open(lua_State* L){lua_pushinteger(L,0);lua_rawsetp(L,LUA_REGISTRYINDEX,&ownerKey);}
int watch(lua_State* L){
    checkKamiExecution(L);
    if(!isKamiCallback(L)||isKamiDrawing(L))return luaL_error(L,"transit.watch requires start, tick or input");
    if(lua_gettop(L)!=1||lua_type(L,1)!=LUA_TSTRING)return luaL_error(L,"transit.watch expects a five-digit Muni stop code");
    size_t n;const char* code=lua_tolstring(L,1,&n);
    if(n!=5||strlen(code)!=n)return luaL_error(L,"invalid stop code");
    const auto token=services::muni::watch(code);
    if(!token)return luaL_error(L,"invalid stop code");
    lua_pushinteger(L,token);lua_rawsetp(L,LUA_REGISTRYINDEX,&ownerKey);
    sync::requestPoll();
    lua_pushboolean(L,true);return 1;
}
void integer(lua_State* L,const char* key,lua_Integer n){lua_pushinteger(L,n);lua_setfield(L,-2,key);}
int snapshot(lua_State* L){
    checkKamiExecution(L);
    if(lua_gettop(L))return luaL_error(L,"transit.snapshot expects no arguments");
    const auto s=services::muni::snapshot();
    if(!s.active||s.generation!=owner(L)){lua_pushnil(L);return 1;}
    lua_newtable(L);lua_pushstring(L,s.stop);lua_setfield(L,-2,"stop");
    lua_pushboolean(L,s.error);lua_setfield(L,-2,"error");
    integer(L,"prediction_timestamp",s.feed.predictionTimestamp);integer(L,"vehicle_timestamp",s.feed.vehicleTimestamp);
    lua_newtable(L);
    for(unsigned i=0;i<s.feed.arrivalCount;++i){const auto& a=s.feed.arrivals[i];lua_newtable(L);integer(L,"epoch",a.epoch);lua_pushstring(L,a.headsign);lua_setfield(L,-2,"headsign");lua_rawseti(L,-2,i+1);}
    lua_setfield(L,-2,"arrivals");lua_newtable(L);
    for(unsigned i=0;i<s.feed.busCount;++i){const auto& b=s.feed.buses[i];lua_newtable(L);lua_pushnumber(L,b.latitude);lua_setfield(L,-2,"lat");lua_pushnumber(L,b.longitude);lua_setfield(L,-2,"lon");integer(L,"timestamp",b.timestamp);lua_rawseti(L,-2,i+1);}
    lua_setfield(L,-2,"buses");return 1;
}
const luaL_Reg functions[]={{"watch",watch},{"snapshot",snapshot},{nullptr,nullptr}};
} const api::Namespace& transitNamespace(){static const api::Namespace ns{"transit",functions,open,close,close};return ns;} }
