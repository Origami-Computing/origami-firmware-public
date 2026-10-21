/**
 * @file api_draw.cpp
 * @brief The kami.draw.* namespace — the widget drawing primitives.
 *
 * Every function appends a node to the "current scene" — a ui::scene::Scene*
 * held in the shared Lua registry by LuaVm::buildScene — so they no-op
 * outside a draw pass. This file owns that scene slot and the setKamiScene
 * accessor. It self-describes as the "draw" namespace via drawNamespace(),
 * which api.cpp wires into the registry.
 */

#include "widgets/api.h"
#include "widgets/api_registry.h"

#include "ui/scene.h"
#include <cstring>

namespace widgets {

namespace {

// Registry is shared by all coroutines. Extra-space pointers would be copied
// on coroutine creation and could retain a stale surface after the draw ends.
static const char kSceneKey=0;
static const char kBitmapPins=0;
inline ui::scene::Scene* curScene(lua_State* L) {
    lua_rawgetp(L,LUA_REGISTRYINDEX,&kSceneKey);
    auto* scene=static_cast<ui::scene::Scene*>(lua_touserdata(L,-1));
    lua_pop(L,1);
    return scene;
}

// Convert a refused scene insertion into a protected Lua draw failure.
int drawResult(lua_State* L, ui::scene::Node* node) {
    return node ? 0 : luaL_error(L, "widget scene capacity exceeded");
}

inline uint16_t color565(lua_Integer v) { return (uint16_t)(v & 0xFFFF); }
inline ui::Rect rectArgs(lua_State* L, int i) {
    return ui::Rect{ (int16_t)luaL_checkinteger(L, i),
                     (int16_t)luaL_checkinteger(L, i + 1),
                     (int16_t)luaL_checkinteger(L, i + 2),
                     (int16_t)luaL_checkinteger(L, i + 3) };
}

// kami.draw.rgb(r, g, b) -> RGB565 integer. Authoring helper so widgets never hand-encode.
int l_rgb(lua_State* L) {
    int r = (int)luaL_checkinteger(L, 1) & 0xFF;
    int g = (int)luaL_checkinteger(L, 2) & 0xFF;
    int b = (int)luaL_checkinteger(L, 3) & 0xFF;
    lua_pushinteger(L, ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    return 1;
}

// kami.draw.rect(id, x, y, w, h, color[, radius])
int l_rect(lua_State* L) {
    ui::scene::Scene* s = curScene(L);
    if (!s) return 0;
    return drawResult(L, s->addRect(luaL_checkstring(L, 1), rectArgs(L, 2),
               color565(luaL_checkinteger(L, 6)),
               (uint8_t)luaL_optinteger(L, 7, 0)));
}

// kami.draw.label(id, x, y, w, h, text[, fg[, bg[, size[, align]]]])
// align: 0 top-left, 1 top-center, 2 middle-center
int l_label(lua_State* L) {
    ui::scene::Scene* s = curScene(L);
    if (!s) return 0;
    const char* text = luaL_checkstring(L, 6);
    uint16_t fg = color565(luaL_optinteger(L, 7, 0xFFFF));
    uint16_t bg = color565(luaL_optinteger(L, 8, 0x0000));
    uint8_t  sz = (uint8_t)luaL_optinteger(L, 9, 1);
    int align   = (int)luaL_optinteger(L, 10, 0);
    if (align < 0 || align > 2) align = 0;
    const int font=luaL_optinteger(L,11,1);
    if(font!=1&&font!=2&&font!=4)return luaL_error(L,"label font must be 1, 2 or 4");
    auto* node=s->addLabel(luaL_checkstring(L, 1), rectArgs(L, 2), text, fg, bg, sz,
                (ui::scene::TextAlign)align);
    if(node)node->font=font;
    return drawResult(L,node);
}

// kami.draw.progress(id, x, y, w, h, value, max[, track[, fill]])
int l_progress(lua_State* L) {
    ui::scene::Scene* s = curScene(L);
    if (!s) return 0;
    uint32_t value = (uint32_t)luaL_checkinteger(L, 6);
    uint32_t maxv  = (uint32_t)luaL_checkinteger(L, 7);
    if (maxv == 0) maxv = 1;
    uint16_t track = color565(luaL_optinteger(L, 8, 0x2104));
    uint16_t fill  = color565(luaL_optinteger(L, 9, 0xFFFF));
    return drawResult(L, s->addProgressBar(luaL_checkstring(L, 1), rectArgs(L, 2), value, maxv, track, fill));
}

// kami.draw.circle(id, cx, cy, r, stroke[, strokeW[, fill[, filled]]])
int l_circle(lua_State* L) {
    ui::scene::Scene* s = curScene(L);
    if (!s) return 0;
    return drawResult(L, s->addCircle(luaL_checkstring(L, 1),
                 (int16_t)luaL_checkinteger(L, 2), (int16_t)luaL_checkinteger(L, 3),
                 (int16_t)luaL_checkinteger(L, 4),
                 color565(luaL_checkinteger(L, 5)),
                 (uint8_t)luaL_optinteger(L, 6, 1),
                 color565(luaL_optinteger(L, 7, 0)),
                 lua_toboolean(L, 8) != 0));
}

// kami.draw.line(id, x0, y0, x1, y1, color)
int l_line(lua_State* L) {
    ui::scene::Scene* s = curScene(L);
    if (!s) return 0;
    return drawResult(L, s->addLine(luaL_checkstring(L, 1),
               (int16_t)luaL_checkinteger(L, 2), (int16_t)luaL_checkinteger(L, 3),
               (int16_t)luaL_checkinteger(L, 4), (int16_t)luaL_checkinteger(L, 5),
               color565(luaL_checkinteger(L, 6))));
}

// Bounded asset decoding outside draw. Avoid thousands of Lua callbacks at startup.
int l_decodeMask(lua_State* L) {
    checkKamiExecution(L);
    if(isKamiDrawing(L))return luaL_error(L,"decode masks before drawing");
    if(lua_gettop(L)<1||lua_gettop(L)>2||lua_type(L,1)!=LUA_TSTRING)
        return luaL_error(L,"decodeMask expects hexadecimal text and optional mask encoding");
    bool rle=false, packed=false;
    if(lua_gettop(L)==2){
        if(lua_type(L,2)!=LUA_TSTRING)return luaL_error(L,"unknown mask encoding");
        size_t n;const char* encoding=lua_tolstring(L,2,&n);
        rle=n==3 && memcmp(encoding,"rle",3)==0;
        packed=n==4 && memcmp(encoding,"2bpp",4)==0;
        if(!rle&&!packed)return luaL_error(L,"unknown mask encoding");
    }
    size_t size;const char* hex=lua_tolstring(L,1,&size);
    if(size%(rle?4:2)||size>67200)return luaL_error(L,"invalid mask size");
    auto digit=[](char c)->int {return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;};
    for(size_t i=0;i<size;++i)if(digit(hex[i])<0)return luaL_error(L,"invalid mask hex digit");
    auto byte=[&](size_t i)->uint8_t {return (digit(hex[i])<<4)|digit(hex[i+1]);};
    size_t output=0;
    if(rle){for(size_t i=0;i<size;i+=4){unsigned count=byte(i);if(!count||count>33600-output)return luaL_error(L,"invalid mask run length");output+=count;}}
    else output=packed?size:size/2;
    if(output>33600)return luaL_error(L,"decoded mask too large");
    luaL_Buffer buffer;char* bytes=luaL_buffinitsize(L,&buffer,output);
    size_t cursor=0;
    for(size_t i=0;i<size;i+=rle?4:2){unsigned count=rle?byte(i):1;uint8_t value=byte(i+(rle?2:0));if(packed){
        bytes[cursor++]=(((value>>6)&3)*5<<4)|(((value>>4)&3)*5);
        bytes[cursor++]=(((value>>2)&3)*5<<4)|((value&3)*5);
    }else{memset(bytes+cursor,value,count);cursor+=count;}}
    luaL_pushresultsize(&buffer,output);checkKamiExecution(L);return 1;
}

// Immutable packed mask; pin the Lua string through scene rendering and GC.
// Row-major, MSB-first, each row padded to whole bytes. No filesystem I/O.
int l_bitmap(lua_State* L) {
    auto* scene=curScene(L);if(!scene)return 0;
    const char* id=luaL_checkstring(L,1);
    auto rect=rectArgs(L,2);
    if(luaL_checkinteger(L,4)<1||luaL_checkinteger(L,4)>280||
       luaL_checkinteger(L,5)<1||luaL_checkinteger(L,5)>240)
        return luaL_error(L,"bitmap destination exceeds screen bounds");
    const auto width=luaL_checkinteger(L,7),height=luaL_checkinteger(L,8);
    if(width<1||width>280||height<1||height>240)return luaL_error(L,"bitmap dimensions exceed screen bounds");
    if(lua_type(L,6)!=LUA_TSTRING)return luaL_error(L,"bitmap pixels must be a packed string");
    size_t size;const auto* pixels=(const uint8_t*)lua_tolstring(L,6,&size);
    const auto bpp=luaL_optinteger(L,11,1);
    if(bpp!=1&&bpp!=4)return luaL_error(L,"bitmap depth must be 1 or 4");
    if(size!=size_t((width*bpp+7)/8)*size_t(height))return luaL_error(L,"bitmap byte count does not match dimensions");
    const auto fg=color565(luaL_checkinteger(L,9)),bg=color565(luaL_checkinteger(L,10));
    lua_rawgetp(L,LUA_REGISTRYINDEX,&kBitmapPins);
    lua_pushvalue(L,6);lua_rawseti(L,-2,scene->count()+1);lua_pop(L,1);
    auto* node=scene->addBitmap(id,rect,pixels,width,height,fg,bg,0,bpp);
    if(node) node->bitmapTransparent=lua_toboolean(L,12) && bpp==4;
    return drawResult(L,node);
}

const luaL_Reg kDrawFns[] = {
    { "decodeMask", l_decodeMask },
    { "bitmap",   l_bitmap },
    { "rgb",      l_rgb },
    { "rect",     l_rect },
    { "label",    l_label },
    { "progress", l_progress },
    { "circle",   l_circle },
    { "line",     l_line },
    { nullptr,    nullptr },
};

// Reset the scene slot when a fresh VM opens.
void drawOnOpen(lua_State* L) { setKamiScene(L,nullptr); lua_createtable(L,ui::scene::Scene::kMaxNodes,0);lua_rawsetp(L,LUA_REGISTRYINDEX,&kBitmapPins); }

}  // namespace

// Wired into the registry by api.cpp (explicit accessor → linker-safe).
const api::Namespace& drawNamespace() {
    static const api::Namespace ns{ "draw", kDrawFns, drawOnOpen, nullptr };
    return ns;
}

bool isKamiDrawing(lua_State* L) { return curScene(L)!=nullptr; }

void setKamiScene(lua_State* L, ui::scene::Scene* scene) {
    lua_pushlightuserdata(L,scene);
    lua_rawsetp(L,LUA_REGISTRYINDEX,&kSceneKey);
}

}  // namespace widgets
