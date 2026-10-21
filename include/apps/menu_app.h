#pragma once

/**
 * @file menu_app.h
 * @brief Main application-launcher menu.
 *
 * MenuApp presents a small hierarchy of app categories. It is pushed onto the
 * stack by the Kami home screen (agent_app) in response to a rotary turn.
 * The first row offers Join Wi-Fi while offline, or Update Firmware when
 * connected and the shared background notice reports a newer release.
 *
 * Input map:
 *   Rotary turn      — scroll the list, play a tick sound on each step
 *   Encoder press    — open the highlighted category or app
 *   LB tap           — return one category; from root, return to Kami
 *   LB+RB chord      — same hierarchy as LB
 */

#include "app.h"
#include "hal/hal_input.h"
#include <TB_TFT_eSPI.h>

namespace scene {
struct Screen;
}

namespace apps {
namespace menu {

/** The App descriptor — pass to NavHandler::push() to open the menu. */
extern App app;
extern const ::scene::Screen kScreenMenu;

void enter();
void exitApp();
void onInput(hal::input::InputEvent ev);
void onDraw(TFT_eSprite& canvas);
/** Returns true when a category consumes the chord, false at the menu root. */
bool onChord();

}  // namespace menu
}  // namespace apps
