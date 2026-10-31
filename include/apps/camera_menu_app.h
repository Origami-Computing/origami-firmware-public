#pragma once
#include "services/photo_storage.h"

/**
 * Camera workflow: live viewfinder, settings/manual/readme views, capture and
 * review. The camera service owns hardware lifetime; UI CameraPreview owns
 * preview pixels/decoding/presentation; photo storage owns naming/upload metadata.
 *
 * VGA capture normally retains the live sensor and saves a fresh owned snapshot.
 * Higher resolutions and low-memory fallback restart the sensor for capture,
 * then restore preview. Photos use /photos/IMG_<sequence>.jpg naming.
 *
 * Entering settings stops the sensor and restores background operations.
 * Committed settings apply on the next viewfinder start. Rotary changes selection
 * or the pending edit; Back cancels an edit or returns to the previous view.
 * Shoot selects Photo or silent Video for this app activation. Video retains a
 * VGA session (QVGA on low-memory fallback), queues JPEGs to VideoRecording,
 * and stores finalized clips in /videos. The Videos app owns preview/playback;
 * video uploads are not implemented.
 * Development builds also offer Video + mic (30s), selected by default: bounded concurrent capture
 * to separate AVI/WAV tracks, with no synchronized on-device audio playback.
 * In the viewfinder: Back exits, B-tap captures or starts/stops video, Enter opens
 * settings. During recording Back/Enter first stop and save. Cover, exit and
 * error drain/finalize the recording before releasing camera admission.
 * In settings/manual: Enter edits/commits or opens a selected subview.
 * In the error view: Back exits; Enter retries camera startup.
 */

#include <TB_TFT_eSPI.h>
#include "app.h"
#include "hal/hal_input.h"

namespace apps {
namespace camera_menu {

/** The App descriptor — pass to NavHandler::push() to open the camera. */
extern App app;
// UI-task tutorial variant. Capture saves an unqueued still, closes Camera and
// returns to the caller. Result is owned file metadata, never a borrowed frame.
void beginTutorialCapture();
services::photos::Target takeTutorialPhoto();


void enter();
void exitApp();
void tick();
void onInput(hal::input::InputEvent ev);
/** Canvas drawing for settings, manual controls and readme views. */
void onDraw(TFT_eSprite& canvas);

#ifdef ORIGAMI_DEV_SERIAL_INPUT
// Thirty-second concurrent mic/video feasibility trial; requires live viewfinder.
bool runAudioTrial();
// One independently restarted VGA trial; -1 selects the production 600 ms settle loop.
bool runSettleTrial(uint32_t runId, int discardFrames, unsigned repeat);
bool copyPreviewForDevelopment(TFT_eSprite& target);
#endif

}  // namespace camera_menu
}  // namespace apps
