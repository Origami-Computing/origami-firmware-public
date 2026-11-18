#pragma once
#include "app.h"
#include "services/photo_storage.h"
namespace apps {
namespace cine_camera {
// The sole Camera app: Home, menu, tutorial, hardware checks and USB tooling.
extern App app;
// UI task: push the same Camera in photo-only tutorial mode.
void beginTutorialCapture();
// Consume the unqueued result after Camera exits; empty when cancelled.
services::photos::Target takeTutorialPhoto();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool runAudioTrial();
#endif
} // namespace cine_camera
} // namespace apps
