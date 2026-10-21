#pragma once

/**
 * @file wifi_portal_app.h
 * @brief WiFi provisioning via soft-AP + captive web portal.
 *
 * The app renders a join QR, portal address and connection status. The
 * provisioning service owns the soft-AP, web server and radio restoration.
 *
 * Coexists with `apps::wifi_setup` (camera-based QR scan); they are wired
 * into Settings as two separate rows so the user can pick whichever flow
 * suits the situation.
 *
 * Input map:
 *   A press   — stop AP+webserver, restore STA, pop back to Settings.
 *   LB+RB chord - onChord nullptr -> NavHandler pops.
 */

#include <TB_TFT_eSPI.h>
#include "app.h"
#include "hal/hal_input.h"

namespace apps {
namespace wifi_portal {

extern App app;

void enter();
void exitApp();
void tick();
void onInput(hal::input::InputEvent ev);
void onDraw(TFT_eSprite& canvas);

}  // namespace wifi_portal
}  // namespace apps
