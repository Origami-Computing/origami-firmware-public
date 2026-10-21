#pragma once
/**
 * @file tennis_app.h
 * @brief Tennis spike (throwaway): swing the kami like a racket. Logic in core/tennis.
 */
#include "app.h"
#include "hal/hal_input.h"
#include <TB_TFT_eSPI.h>

namespace apps {
namespace tennis {
extern App app;
void tick();
void onInput(hal::input::InputEvent ev);
void onDraw(TFT_eSprite& canvas);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
// Dev: `tennis play L R M ...` queues where the next shots land (the kami swings with the timing that puts
// them there, and serves for power); `tennis auto on|off` aims every shot away from the AI; `tennis help [PAGE]` opens/closes the
// how-to-play panel; `tennis status`.
void devCommand(const char* args, Print& out);
#endif
}  // namespace tennis
}  // namespace apps
