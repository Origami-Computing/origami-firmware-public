#pragma once

/**
 * @file lua_probe.h
 * @brief Phase-0 de-risk spike for the Lua widget runtime.
 *
 * Spins up a sandboxed Lua 5.4 VM (base/string/math/table/utf8/coroutine only),
 * runs a trivial script, and logs heap/PSRAM deltas + timing over Serial. The
 * purpose is purely to measure the flash/RAM/latency cost of embedding Lua
 * before any real runtime is built on top. Compiled in only when LUA_PROBE is
 * defined; safe to call once from setup().
 */

namespace widgets {

/** Create a sandboxed state, run a small script, log cost, and tear down. */
void probeRun();

}  // namespace widgets
