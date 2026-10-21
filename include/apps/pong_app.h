#pragma once

/**
 * @file pong_app.h
 * @brief Single-player Pong for the menu firmware.
 *
 * Input map:
 *   Rotary      - move the player paddle
 *   B tap/press - serve; after a point, resume play
 *   A tap       - reset match
 *   C tap       - pause / resume
 *   A+B chord   - NavHandler pops back to the menu
 */

#include "app.h"
#include "hal/hal_input.h"
#include <TB_TFT_eSPI.h>

namespace apps {
namespace pong {

extern App app;

void tick();
void onInput(hal::input::InputEvent ev);
void onDraw(TFT_eSprite& canvas);

}  // namespace pong
}  // namespace apps
