#pragma once

/**
 * @file wifi_setup_app.h
 * @brief WiFi provisioning by scanning a QR code with the camera.
 *
 * Direct-panel app (onDraw = nullptr): live camera preview with periodic QR
 * decode. Payload parsing uses core::wifi_qr; the QR session delegates
 * joining and credential persistence to the Wi-Fi manager.
 *
 * QR payload formats are documented in include/core/wifi_qr_parser.h.
 *
 * Input map:
 *   Scanning is automatic.
 *   B tap — skip scanning (the caller handles the skip explanation)
 *   Encoder tap — scan again after failure / dismiss success
 *   Left tap  — exit and return to the caller
 *   LB+RB chord - onChord nullptr -> NavHandler pops
 */

#include <TB_TFT_eSPI.h>
#include "app.h"
#include "apps/onboarding_flow.h"
#include "hal/hal_input.h"

namespace apps {
namespace wifi_setup {

extern App app;
void beginTutorial();
bool tutorialActive();
onboarding::WifiResult takeTutorialResult();

void exitApp();
void tick();
void onInput(hal::input::InputEvent ev);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
// Exercise the post-decode flow without exposing stored passwords over USB.
bool debugJoin(bool savedNetwork);
#endif

}  // namespace wifi_setup
}  // namespace apps
