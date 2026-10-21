#pragma once

/**
 * @file pomodoro_app.h
 * @brief Focus/break timer with shared field editing and synthesized alarm sounds.
 *
 * Input map:
 *   A prepares the next interval without starting it; B starts/pauses/resumes.
 *   C opens settings; rotary selects/adjusts fields and C toggles editing.
 *   A+B leaves settings or returns to the menu.
 */

#include "app.h"
#include "hal/hal_input.h"
#include <TB_TFT_eSPI.h>

namespace apps {
namespace pomodoro {

extern App app;

void exitApp();
void tick(uint32_t now);
void onInput(hal::input::InputEvent ev);
void onDraw(TFT_eSprite& canvas);
bool isActive();
bool isAlerting();
// Called even when another screen covers the app, to release alert outputs.
void serviceFeedback(uint32_t now);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
void printStatus();
bool shortenForTest(uint32_t seconds);
#endif

}  // namespace pomodoro
}  // namespace apps
