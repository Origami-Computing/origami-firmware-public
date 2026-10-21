#pragma once

/**
 * @file voice_recorder_app.h
 * @brief Continuous audio recorder that saves WAV files to the SD card.
 *
 * Architecture
 * ------------
 * The app bypasses the canvas pipeline for its lifetime (enterOptimizedRender)
 * and draws directly to the TFT hardware.  This avoids contention on the
 * shared SPI bus: SD writes (audio data) and TFT writes (UI) cannot overlap —
 * direct TFT access lets the app paint during the gaps between SD blocks.
 *
 * Audio pipeline:
 *   I²S mic (PDM) → hal::audio::readMic → int32 raw samples →
 *   right-shift to int16 → WavWriter (writes 16-bit PCM to SD)
 *
 * Each A press closes the current WAV file, stops the mic, and opens a new
 * file so the user can record multiple clips without leaving the app.  Files
 * are named by a millisecond counter (no RTC dependency).
 *
 * States:
 *   RECORDING — mic active, writing to SD, VU meter animating
 *   PAUSED    — mic stopped, file closed, ready for a new recording
 *
 * Input map:
 *   A press   — in RECORDING: stop and save  |  in PAUSED: start new recording
 *   A+B chord — onChord nullptr → NavHandler pops (mic stopped in onExit)
 */

#include "app.h"
#include "hal/hal_input.h"

namespace apps {
namespace voice_recorder {

/** The App descriptor — pass to NavHandler::push() to open the recorder. */
extern App app;

/** True while the now-playing UI is drawing directly to the TFT hardware
 *  (canvas pipeline bypassed).  NavHandler::draw() checks this. */
bool usesDirectPanel();

void enter();
void exitApp();
void tick();
void onInput(hal::input::InputEvent ev);

/** True while actively writing audio samples to SD (not paused).
 *  The app runtimePolicy uses this to inhibit idle return and battery sleep
 *  and report activity while capture is active. */
bool isRecording();

#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool developmentUi(const char* command);
#endif

}  // namespace voice_recorder
}  // namespace apps
