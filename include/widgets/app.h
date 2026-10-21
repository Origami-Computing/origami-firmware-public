#pragma once

/**
 * @file lua_app.h
 * @brief Host app that runs a sandboxed Lua widget as a first-class app.
 *
 * One native App owns picker, loading and runner workflows. Explicit app
 * transitions own LuaVm lifetime; screen callbacks present views. The widget
 * supplies state, input and timers and emits a ui::scene::Scene via kami.*.
 * LB and the LB+RB chord remain host-reserved escape controls.
 */

#include <app.h>   // base App descriptor (include/app.h). Angle brackets so it
                    // resolves via the include path, not to this widgets/app.h.

namespace widgets {

extern App app;

/** UI-task observations of runner/loading drawing; counters wrap naturally. */
struct DrawStats { uint32_t frames, elapsedUs; };
DrawStats drawStats();

/** Switch the app to its loading screen (push-driven launch flow). Call only
 *  after this app is on the nav stack. */
void showLoading();

}  // namespace widgets
