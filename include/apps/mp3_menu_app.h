#pragma once

/**
 * @file mp3_menu_app.h
 * @brief Software MP3 player with a file-browser and a now-playing screen.
 *
 * Architecture overview
 * ---------------------
 * The app has two sub-states:
 *
 *   MP3_BROWSER   — Canvas app; renders a scrollable list of .mp3 files from
 *                   the /music/ directory on the SD card.
 *
 *   MP3_PLAYING   — Direct-panel app; bypasses the canvas pipeline and writes
 *                   the now-playing UI (title, progress bar, VU meter bars)
 *                   straight to the TFT hardware.  The canvas pipeline is
 *                   re-enabled when the user returns to the browser.
 *
 * Decode pipeline
 * ---------------
 * A dedicated FreeRTOS task on core 0 (mp3PlaybackTaskFn) runs at priority 5
 * and continuously decodes the MP3 stream from SD into a PSRAM PCM ring
 * buffer, then drains the ring into the I²S DMA TX FIFO.  This keeps the
 * Arduino main loop (core 1) free for UI work without audio stuttering.
 * End-of-track detection (ring drained after decoder EOF) is flagged via
 * mp3WantsNextTrack; the main tick() picks it up and advances the playlist.
 *
 * Chord behaviour
 * ---------------
 *   Now-playing screen  — chord stops playback and switches back to browser
 *                         (returns true, app stays mounted).
 *   Browser             — chord returns false, NavHandler pops the app.
 *
 * Input map (browser):
 *   Rotary    — scroll file list
 *   Encoder press - start playing highlighted file
 *
 * Input map (now-playing):
 *   Rotary    — adjust volume (0–100, ±5 per detent)
 *   Encoder press - pause / resume
 *   RB tap        - skip to next track (end-of-track also auto-advances)
 */

#include "app.h"
#include <TB_TFT_eSPI.h>
#include "hal/hal_input.h"

namespace apps {
namespace mp3_menu {

/** The App descriptor — pass to NavHandler::push() to open the music player. */
extern App app;

void enter();
void exitApp();
/** See chord behaviour in the file header. */
bool onChord();
void tick();
void onInput(hal::input::InputEvent ev);
/** Only called in browser sub-state; now-playing draws directly to hardware. */
void onDraw(TFT_eSprite& canvas);

/** True while the now-playing screen is active (canvas pipeline bypassed).
 *  NavHandler::draw() checks this to decide whether to flush the canvas. */
bool usesDirectPanel();

/** True while audio is actively streaming (file open and not paused).
 *  The app runtimePolicy uses this to inhibit idle return and battery sleep
 *  and report activity until playback pauses, ends or leaves now-playing. */
bool isPlaying();

/** Read-only production state for device regression checks. */
void dumpStatus(Print& out);

}  // namespace mp3_menu
}  // namespace apps
