#pragma once

/**
 * @file hardware_test_app.h
 * @brief Guided factory / field hardware self-test.
 *
 * The test walks through a fixed sequence of steps, each verifying one
 * hardware subsystem.  Steps advance automatically when a test passes or
 * manually via A/B press.  A PASS/FAIL result is shown for each step.
 *
 * Test sequence:
 *   Display → Button A → Button B → Vibration → SD Card → Audio →
 *   Camera → IMU Level → Recording → Complete.
 * The current Vibration/Audio A-to-advance guards prevent normal progression
 * beyond those steps; development diagnostics can select later steps directly.
 *
 * Each step writes a one-line result ("Button B works!" / "SD FAILED")
 * into a static buffer displayed by onDraw().  The canvas Screen primitives
 * are used throughout except for the display colour-fill step which calls
 * raw() to paint full-screen solid colours.
 *
 * Input map:
 *   A press/hold  — step-specific (hold A to confirm mic/camera steps)
 *   A release     — confirm hold-gated steps
 *   B press       — on TEST_COMPLETE or TEST_DISPLAY: pop back to Settings
 *                   on other steps: retreat to the previous step
 *   A+B chord     — onChord nullptr → NavHandler pops (same as B on complete)
 */

#include <TB_TFT_eSPI.h>
#include "app.h"
#include "hal/hal_input.h"

namespace apps {
namespace hardware_test {

/** The App descriptor — pass to NavHandler::push() to begin the test sequence. */
extern App app;

#ifdef ORIGAMI_DEV_SERIAL_INPUT
// Fresh Display step only; refuses existing recording destinations. Does not
// change normal input navigation. Use production A press/release after selection.
bool selectDiagnosticStep(const char* step, uint32_t recordingIndex = 0);
void dumpDiagnosticStatus(Print& output);
#endif

void enter();
/** Stop any active tone, vibration, mic, camera, or I²S TX on exit. */
void exitApp();
void tick();
void onInput(hal::input::InputEvent ev);
void onDraw(TFT_eSprite& canvas);

}  // namespace hardware_test
}  // namespace apps
