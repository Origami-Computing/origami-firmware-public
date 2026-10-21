#pragma once

#include "app.h"
#include "hal/hal_input.h"
#include <TB_TFT_eSPI.h>

namespace apps {
namespace ota_update {

extern App app;

void enter();
void exitApp();
void tick();
bool onChord();
void onInput(hal::input::InputEvent ev);
void onDraw(TFT_eSprite& canvas);

// Returns true while OTA is checking, downloading, or flashing.
bool isActive();
// Asynchronous, throttled Settings notice; never installs automatically.



}  // namespace ota_update
}  // namespace apps
