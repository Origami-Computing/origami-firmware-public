#include "services/camera_session.h"
#include <Arduino.h>
#include <esp_camera.h>

namespace services { namespace camera {
bool Session::initializePreview(int frameSize, const hal::camera::Tuning& tuning,
                                uint32_t settleMs, bool (&prepareConsumer)()) {
    if (!start(frameSize, PIXFORMAT_JPEG, &tuning) || !prepareConsumer()) {
        stop();
        return false;
    }
    settle(settleMs);
    return true;
}

bool Session::openPreview(int frameSize, const hal::camera::Tuning& tuning,
                          bool (&prepareConsumer)()) {
    delay(100);
    // Preserve first-entry AE/AWB convergence before exposing sensor frames.
    return initializePreview(frameSize, tuning, 700, prepareConsumer);
}

bool Session::restartPreview(int frameSize, const hal::camera::Tuning& tuning,
                             uint32_t settleMs, bool (&prepareConsumer)()) {
    stop();
    delay(50);
    if (!initializePreview(frameSize, tuning, settleMs, prepareConsumer)) return false;
    hal::camera::CameraFrame warmup;
    if (read(&warmup)) returnFrame();
    return true;
}
} }
