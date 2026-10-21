#pragma once

/**
 * @file dumb_flappy_bird_app.h
 * @brief Voice-controlled Flappy Bird style game.
 *
 * Input map:
 *   yell louder - fly higher
 *   yell softer - drift lower
 *   A tap       - restart
 *   C tap       - start / restart / pause / resume
 *   B tap       - measure room noise; gameplay stays stopped
 *   A+B chord   - NavHandler pops back to the menu
 */

#include "app.h"
#include "hal/hal_input.h"
#include <TB_TFT_eSPI.h>

namespace apps {
namespace dumb_flappy_bird {

extern App app;

void enter();
void exitApp();
void tick();
void onInput(hal::input::InputEvent ev);
void onDraw(TFT_eSprite& canvas);
bool isActive();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
void printStatus(Print& out);
void testNoise(Print& out);
#endif

}  // namespace dumb_flappy_bird
}  // namespace apps
