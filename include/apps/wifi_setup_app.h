#pragma once

/**
 * @file wifi_setup_app.h
 * @brief WiFi provisioning from camera QR or private GGWave audio packets.
 *
 * Direct-panel app (onDraw = nullptr): live camera preview with periodic QR
 * decode. Payload parsing uses core::wifi_qr; the QR session delegates
 * joining and credential persistence to the Wi-Fi manager.
 *
 * QR payload formats are documented in include/core/wifi_qr_parser.h.
 * Tutorial Audio receives W1 packets through ggwave_receiver, reassembles them
 * with core::wifi_audio, then releases the microphone before using the existing
 * QR session's radio/join/result owner (without starting its camera/decoder).
 * Audio A returns to method selection; B continues/skips. Cover/exit cancels
 * reception, clears pending credential data and releases the session.
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
// Audio uses the same credential parser/join/results, with private GGWave input.
void beginTutorial(bool audio=false);
// Direct Settings entry; same audio view/controls, returns to the calling menu.
void beginAudio();
bool tutorialActive();
onboarding::WifiResult takeTutorialResult();

void exitApp();
void tick();
void onInput(hal::input::InputEvent ev);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
// Exercise the post-decode flow without exposing stored passwords over USB.
bool debugJoin(bool savedNetwork, bool accountPayload=false);
// Host-decoded visible QR, injected into the normal parser; never logs payload.
bool debugPayload(const char* payload);
void printStatus(); // Redacted workflow state only; no credential payloads.
#endif

}  // namespace wifi_setup
}  // namespace apps
