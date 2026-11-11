#pragma once
#include "app.h"
#include "hal/hal_input.h"
#include <TB_TFT_eSPI.h>
class UploadQueue;

namespace apps {
namespace settings_menu {
void openWifiMenu();

extern App app;

void exitApp();
void tick();
bool onChord();
void onInput(hal::input::InputEvent ev);
void onDraw(TFT_eSprite& canvas);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
void setUploadPreviewForDevelopment(UploadQueue* queue);
#endif

}  // namespace settings_menu
}  // namespace apps
