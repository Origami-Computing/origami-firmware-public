#pragma once

/**
 * @file lua_vm.h
 * @brief Sandboxed Lua VM manager for a single widget.
 *
 * Owns one lua_State whose heap uses capped PSRAM allocations with a hard byte
 * ceiling, and whose every entry point is wrapped in a protected call guarded
 * by an instruction-count budget. Memory, instruction and scene-capacity
 * failures surface as faults, along with script errors. Scene overflow faults
 * the whole draw pass even when a script catches the insertion error.
 *
 * The VM exposes safe Lua libraries and `kami.*` (see api.h). Device access
 * is limited to host bindings; scripts receive no raw device handles.
 */

#include <stddef.h>
#include <stdint.h>

struct lua_State;
struct lua_Debug;

namespace ui { namespace scene { class Scene; } }

namespace widgets {

class LuaVm {
public:
    /** Create the state, open the safe stdlib + kami API, install guards.
     *  @param memCeilingBytes hard cap on Lua heap (PSRAM); allocations past it
     *         fail and raise a catchable Lua OOM. Returns false on init failure. */
    bool open(size_t memCeilingBytes);

    /** Tear down the VM and free its allocations. Safe to call when not open. */
    void close();

    bool isOpen() const { return L_ != nullptr; }

    /** Compile + run a chunk that populates the global `widget` table.
     *  A fresh empty `widget` table is provided before the chunk runs.
     *  Returns false (and sets faulted) on syntax/runtime/over-budget error. */
    bool loadScript(const char* src, size_t len, const char* chunkName);

    /** Set kami.data from a JSON object (agent-supplied widget data). Must be
     *  called BEFORE loadScript — scripts may read kami.data at top level.
     *  Non-object/invalid JSON leaves kami.data as the empty table installed
     *  at open(). Returns true if the JSON was applied. */
    bool setDataJson(const char* json, size_t len);

    // Widget lifecycle — each is a no-op if the widget omits that callback.
    // All return false if the call faulted (see error()).
    bool callStart();
    bool callTick(uint32_t now);
    bool callInput(const char* type, int32_t delta);
    bool callChord(bool& handledOut);
    void callStop();

    /** Call widget.draw(now) with `out` as the active render target; the kami
     *  drawing functions append nodes to it. Returns false on script or capacity
     *  fault; the caller must replace the incomplete scene with an error view. */
    bool buildScene(ui::scene::Scene& out, uint32_t now, int16_t w, int16_t h);

    bool        faulted() const { return faulted_; }
    const char* error()   const { return errBuf_; }

    size_t liveBytes() const { return live_; }
    size_t peakBytes() const { return peak_; }

private:
    // Allocator hook (lua_Alloc); ud is `this`. Enforces the ceiling + tallies.
    static void* allocHook(void* ud, void* ptr, size_t osize, size_t nsize);

    static void countHook(lua_State* state, lua_Debug*);
    void armBudget(uint32_t elapsedLimitMs=100);
    friend void checkKamiExecution(lua_State* state);
    friend bool isKamiCallback(lua_State* state);
    bool inCallback_=false;
    uint32_t budgetStartedMs_=0, elapsedLimitMs_=100;
    int instructionsRemaining_=0;
    bool budgetExceeded_=false;

    struct Invocation;
    static int invokeProtected(lua_State* state);
    bool invoke(const Invocation& call);
    void setFault(const char* msg);

    lua_State* L_      = nullptr;
    size_t     live_   = 0;
    size_t     peak_   = 0;
    size_t     ceiling_ = 0;
    bool       faulted_ = false;
    char       errBuf_[160] = {};
};

}  // namespace widgets
