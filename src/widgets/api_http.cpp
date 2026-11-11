#include "widgets/api.h"
#include "widgets/api_registry.h"
#include "widgets/json_value.h"
#include "widgets/sync.h"
#include "services/app_http.h"
#include "core/bounded_json_memory.h"
#include <cstring>
#include <new>
namespace widgets { namespace {
const char ownerKey=0;
uint32_t owner(lua_State* L){lua_rawgetp(L,LUA_REGISTRYINDEX,&ownerKey);auto n=uint32_t(lua_tointeger(L,-1));lua_pop(L,1);return n;}
void close(lua_State* L){services::app_http::stop(owner(L));}
void open(lua_State* L){lua_pushinteger(L,0);lua_rawsetp(L,LUA_REGISTRYINDEX,&ownerKey);}
void mutation(lua_State* L){checkKamiExecution(L);if(!isKamiCallback(L)||isKamiDrawing(L))luaL_error(L,"http requires start, tick or input");}
void string(lua_State* L,int index,char* out,size_t capacity){
 if(lua_type(L,index)!=LUA_TSTRING)luaL_error(L,"HTTP fields must be strings");
 size_t n;const char* value=lua_tolstring(L,index,&n);if(n>=capacity||memchr(value,0,n))luaL_error(L,"HTTP field too long or contains NUL");memcpy(out,value,n);out[n]=0;
}
int submit(lua_State* L,services::app_http::Request& request){
 auto token=services::app_http::request(request);if(!token){lua_pushnil(L);lua_pushliteral(L,"Invalid HTTP request or unavailable memory");return 2;}
 lua_pushinteger(L,token);lua_rawsetp(L,LUA_REGISTRYINDEX,&ownerKey);sync::requestPoll();lua_pushboolean(L,true);return 1;
}
int get(lua_State* L){mutation(L);if(lua_gettop(L)!=1)return luaL_error(L,"http.get expects URL");
 auto* r=new(lua_newuserdatauv(L,sizeof(services::app_http::Request),0))services::app_http::Request{};string(L,1,r->url,sizeof(r->url));return submit(L,*r);
}
int request(lua_State* L){mutation(L);if(lua_gettop(L)!=1||lua_type(L,1)!=LUA_TTABLE)return luaL_error(L,"http.request expects a table");
 auto* r=new(lua_newuserdatauv(L,sizeof(services::app_http::Request),0))services::app_http::Request{};
 lua_getfield(L,1,"url");string(L,-1,r->url,sizeof(r->url));lua_pop(L,1);
 lua_getfield(L,1,"method");if(!lua_isnil(L,-1))string(L,-1,r->method,sizeof(r->method));lua_pop(L,1);
 lua_getfield(L,1,"body");if(!lua_isnil(L,-1)){if(lua_type(L,-1)!=LUA_TSTRING)return luaL_error(L,"body must be a string");size_t n;const char* body=lua_tolstring(L,-1,&n);if(n>sizeof(r->body))return luaL_error(L,"HTTP body exceeds 4096 bytes");memcpy(r->body,body,n);r->bodySize=n;}lua_pop(L,1);
 lua_getfield(L,1,"timeout_ms");if(!lua_isnil(L,-1)){if(!lua_isinteger(L,-1))return luaL_error(L,"timeout_ms must be an integer");auto n=lua_tointeger(L,-1);if(n<1000||n>30000)return luaL_error(L,"timeout_ms must be 1000..30000");r->timeoutMs=uint32_t(n);}lua_pop(L,1);
 lua_getfield(L,1,"headers");if(!lua_isnil(L,-1)){if(lua_type(L,-1)!=LUA_TTABLE)return luaL_error(L,"headers must be a table");lua_pushnil(L);while(lua_next(L,-2)){
  if(r->headerCount>=services::app_http::kMaxHeaders)return luaL_error(L,"At most 8 HTTP headers");auto& h=r->headers[r->headerCount++];string(L,-2,h.name,sizeof(h.name));string(L,-1,h.value,sizeof(h.value));lua_pop(L,1);
 }}lua_pop(L,1);return submit(L,*r);
}
int cancel(lua_State* L){mutation(L);if(lua_gettop(L))return luaL_error(L,"http.cancel expects no arguments");close(L);return 0;}
int take(lua_State* L){mutation(L);if(lua_gettop(L))return luaL_error(L,"http.take expects no arguments");
 if(!services::app_http::ready(owner(L))){lua_pushnil(L);return 1;}
 auto* response=new(lua_newuserdatauv(L,sizeof(services::app_http::Response),0))services::app_http::Response{};
 if(!services::app_http::take(owner(L),*response)){lua_pop(L,1);lua_pushnil(L);return 1;}
 lua_newtable(L);lua_pushinteger(L,response->status);lua_setfield(L,-2,"status");lua_pushboolean(L,response->status>=200&&response->status<300);lua_setfield(L,-2,"ok");
 lua_pushlstring(L,response->body,response->size);lua_setfield(L,-2,"text");
 int result=LUA_OK;bool decoded=false;
 if(response->size){core::BoundedJsonMemory memory(49152);JsonDocument doc(&memory);
  if(!deserializeJson(doc,response->body,response->size,DeserializationOption::NestingLimit(8))){lua_pushcfunction(L,json::protectedPush);lua_pushlightuserdata(L,&doc);result=lua_pcall(L,1,1,0);decoded=result==LUA_OK;}
 }
 if(result!=LUA_OK)return lua_error(L);if(decoded)lua_setfield(L,-2,"data");
 if(response->status<0){lua_pushstring(L,response->status==-3?"Request timed out":response->status==-2?"Response exceeds limit or transfer failed":"Network request failed");lua_setfield(L,-2,"error");}
 return 1;
}
const luaL_Reg functions[]={{"get",get},{"request",request},{"take",take},{"cancel",cancel},{nullptr,nullptr}};
}const api::Namespace& httpNamespace(){static const api::Namespace ns{"http",functions,open,close,close};return ns;}}
