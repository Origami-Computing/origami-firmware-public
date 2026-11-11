#pragma once

/**
 * @file doodle_jump_app.h
 * @brief Doodle Jump control prototype for validating IMU roll steering.
 *
 * Input map:
 *   C tap     - start/restart, or pause/resume an active game
 *   B tap     - shoot while running (ignored while paused)
 *   A tap     - reinitialize neutral roll position while running
 *   A+B chord - NavHandler pops back to the menu
 */

#include "app.h"
#include "core/health_policy.h"
#include "hal/hal_input.h"
#include <TB_TFT_eSPI.h>

namespace apps {
namespace doodle_jump {

extern App app;

void tick();
void onInput(hal::input::InputEvent ev);
void onDraw(TFT_eSprite& canvas);
// Tilt-only play: busy while running with a tilt change or button within 60 s.
core::health::RuntimePolicy runtimePolicy();

#ifdef ORIGAMI_DEV_SERIAL_INPUT
void printStatus(Print& out);
void testFall();
void testClipping();
void testTilt(float offsetDeg);  // Offsets measured roll until app exit.
#endif

}  // namespace doodle_jump
}  // namespace apps
