#pragma once

/**
 * @file trex_app.h
 * @brief Chrome-style endless T-Rex runner for the menu firmware.
 *
 * Input map:
 *   B press/release - jump / shorten jump; starts or restarts outside play
 *   A hold    - duck on ground / fast fall in air
 *   C tap     - start / restart / pause / resume
 *   A+B chord - NavHandler pops back to the menu
 */

#include "app.h"
#include "hal/hal_input.h"
#include <TB_TFT_eSPI.h>

namespace apps {
namespace trex {

extern App app;

void tick();
void onInput(hal::input::InputEvent ev);
void onDraw(TFT_eSprite& canvas);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
void printStatus(Print& out);
void testPhysics(Print& out);
#endif

}  // namespace trex
}  // namespace apps
