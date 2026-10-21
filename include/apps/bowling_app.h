#pragma once
/**
 * @file bowling_app.h
 * @brief Wii-Sports-style motion bowling. Logic in core/bowling, drawing in games/bowling_view.
 */
#include "app.h"
#include "hal/hal_input.h"
#include <TB_TFT_eSPI.h>

namespace apps {
namespace bowling {
extern App app;
void tick();
void onInput(hal::input::InputEvent ev);
void onDraw(TFT_eSprite& canvas);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
// Dev: `bowl throw [speed m/s] [headingDeg] [twistDeg]` throws without swinging (from the current position/aim);
// `bowl aim <deg>`, `bowl pos <boards>`, `bowl new`, `bowl help` (opens/closes how to play), `bowl status`.
void devCommand(const char* args, Print& out);
#endif
}  // namespace bowling
}  // namespace apps
