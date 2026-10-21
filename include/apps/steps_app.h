#pragma once

/**
 * @file steps_app.h
 * @brief Daily step counter using the LSM6DSL hardware pedometer.
 *
 * The steps service owns sensor sampling, daily rollover and persistence.
 * This app presents its snapshot and translates input into service commands.
 *
 * Input map:
 *   A press    — reset today's count to zero
 *   Rotary     — adjust the daily goal (±500 steps per detent, 1000–30000)
 *   A+B chord  — onChord nullptr → NavHandler pops back to menu
 */

#include <stdint.h>
#include "app.h"
#include "hal/hal_input.h"
#include <TB_TFT_eSPI.h>

namespace scene {
struct Screen;
}

namespace apps {
namespace steps {

/** The App descriptor — pass to NavHandler::push() to open the step counter. */
extern App app;
extern const ::scene::Screen kScreenSteps;

void tick();
void onInput(hal::input::InputEvent ev);
void onDraw(TFT_eSprite& canvas);

}  // namespace steps
}  // namespace apps
