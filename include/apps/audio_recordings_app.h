#pragma once
/**
 * @file audio_recordings_app.h
 * @brief WAV player app for clips saved by the Audio Recorder.
 */

#include "app.h"
#include <TB_TFT_eSPI.h>
#include "hal/hal_input.h"

namespace apps {
namespace audio_recordings {

extern App app;

void enter();
void exitApp();
bool onChord();
void tick();
void onInput(hal::input::InputEvent ev);
void onDraw(TFT_eSprite& canvas);
bool usesDirectPanel();
bool isPlaying();

}  // namespace audio_recordings
}  // namespace apps
