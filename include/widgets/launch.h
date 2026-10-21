#pragma once

/**
 * @file launch.h
 * @brief State bridge for the server-pushed "talk -> loading -> launch" flow.
 *
 * When the Kami agent generates a widget it sends widget frames over the agent
 * WebSocket (widget_v1): widget_loading before generation, widget_ready when the
 * widget is saved, widget_error on failure. services::agent_inbox hands those to
 * the notify* setters here on ui_task; the Lua Widget app + the ui_task main loop
 * poll the take* getters to drive the UI:
 *
 *   widget_loading -> main loop pushes the Lua app + shows the loading screen
 *   widget_ready   -> loading screen syncs just that widget and auto-launches it
 *   widget_error   -> loading screen shows the error, returns to the picker
 *
 * All setters/getters run on ui_task (agent_inbox::poll, the main loop, and the
 * app tick are all ui_task), so plain statics suffice — no volatile / atomics.
 */

#include <stddef.h>

namespace widgets {
namespace launch {

// ── Setters — called by the automation action handler ───────────────────────
void notifyLoading(const char* title);
void notifyReady(const char* id, const char* slug);
void notifyError(const char* msg);

// ── Getters — polled on ui_task ─────────────────────────────────────────────

/** True once per widget_loading push; the main loop uses it to navigate to the
 *  Lua app's loading screen. Clears on read. */
bool takeLoadingRequest();

/** The title from the most recent widget_loading (for the loading screen to
 *  display). Valid until the next loading push; empty if none. */
const char* title();

/** True once when a widget_ready push arrived; fills id/slug. Clears on read. */
bool takeReady(char* idOut, size_t idCap, char* slugOut, size_t slugCap);

/** True once when a widget_error push arrived; fills msg. Clears on read. */
bool takeError(char* msgOut, size_t msgCap);

}  // namespace launch
}  // namespace widgets
