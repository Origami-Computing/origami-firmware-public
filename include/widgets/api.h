#pragma once

/**
 * @file kami_api.h
 * @brief The `kami.*` host API exposed to widget scripts.
 *
 * This is the entire surface a Lua widget can touch. Drawing functions append
 * nodes to the "current scene" — a ui::scene::Scene* stashed in the lua_State's
 * shared registry by LuaVm::buildScene — so they are no-ops if called outside a
 * draw pass. A refused insertion raises a Lua error; the host rejects an
 * overflowed frame even if the script catches that error. Device operations use the native control boundary; scripts receive no raw
 * hardware, filesystem or network handles.
 */

struct lua_State;

namespace ui { namespace scene { class Scene; } }

namespace widgets {

/** Register the global `kami` table on L. Call once at VM open. */
void registerKamiApi(lua_State* L);

/** Set/clear the scene that kami drawing functions target this frame. */
void setKamiScene(lua_State* L, ui::scene::Scene* scene);

// Host bindings check this before potentially slow work; failures poison the callback.
void checkKamiExecution(lua_State* L);
bool isKamiCallback(lua_State* L); // False during script loading and native VM teardown.

bool isKamiDrawing(lua_State* L); // Shared across coroutines; host-controlled.

/** Update kami.W / kami.H for the current surface size. */
void setKamiSurfaceSize(lua_State* L, int width, int height);

}  // namespace widgets
