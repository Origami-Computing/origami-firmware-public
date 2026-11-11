#include "widgets/api.h"
#include "widgets/api_registry.h"
#include "control/device_api.h"
namespace widgets { namespace {
char ownerKey;
void open(lua_State* L){lua_pushboolean(L,0);lua_rawsetp(L,LUA_REGISTRYINDEX,&ownerKey);}
void stop(lua_State* L){
 lua_rawgetp(L,LUA_REGISTRYINDEX,&ownerKey);bool owned=lua_toboolean(L,-1);lua_pop(L,1);
 if(!owned)return;
 control::Request r;r.operation=control::Operation::HapticStop;control::execute(r);
 lua_pushboolean(L,0);lua_rawsetp(L,LUA_REGISTRYINDEX,&ownerKey);
}
int play(lua_State* L){
 checkKamiExecution(L);
 if(isKamiDrawing(L)||!isKamiCallback(L)){lua_pushboolean(L,0);return 1;}
 control::Request r;r.operation=control::Operation::HapticPlay;r.cue=luaL_checkstring(L,1);
 bool ok=control::execute(r).error==control::Error::None;
 if(ok){lua_pushboolean(L,1);lua_rawsetp(L,LUA_REGISTRYINDEX,&ownerKey);}
 lua_pushboolean(L,ok);return 1;
}
int halt(lua_State* L){checkKamiExecution(L);if(isKamiDrawing(L)||!isKamiCallback(L)){lua_pushboolean(L,0);return 1;}stop(L);lua_pushboolean(L,1);return 1;}
const luaL_Reg functions[]={{"play",play},{"stop",halt},{nullptr,nullptr}};
}
const api::Namespace& hapticsNamespace(){static const api::Namespace ns{"haptics",functions,open,stop,stop};return ns;}
}
