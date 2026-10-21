#include "services/camera_session.h"
#include <esp_camera.h>
#include <Arduino.h>
namespace services { namespace camera {
const char* captureLabel(CaptureResolution requested) {
    switch (requested) {
        case CaptureResolution::Hd: return "HD 1280x720";
        // This persisted request is clamped by the installed sensor. Do not
        // promise 5MP dimensions on boards whose sensor delivers 3MP.
        case CaptureResolution::FiveMp: return "Max resolution";
        default: return "VGA 640x480";
    }
}
CaptureResolution maxCaptureResolution() {
    const size_t kb=hal::camera::availablePsramKb();
    if (kb>=1040u) return CaptureResolution::FiveMp;
    if (kb>=260u) return CaptureResolution::Hd;
    return CaptureResolution::Vga;
}
namespace {
framesize_t frameSize(CaptureResolution resolution) {
    switch (resolution) {
        case CaptureResolution::Hd: return FRAMESIZE_HD;
        case CaptureResolution::FiveMp: return FRAMESIZE_QSXGA;
        default: return FRAMESIZE_VGA;
    }
}
}
Session::StillResult Session::prepareStill(CaptureResolution requested, const hal::camera::Tuning& tuning) {
    stop();
    delay(80);
    const bool deep=requested>=CaptureResolution::Hd;
    const auto resources=prepareCapture(deep);
    if (resources==PrepareResult::CameraBusy) return StillResult::CameraBusy;
    if (resources==PrepareResult::BackgroundBusy) return StillResult::BackgroundBusy;
    if (resources==PrepareResult::AgentBusy) return StillResult::AgentBusy;
    delay(deep?120:20);
    auto target=frameSize(requested);
    const auto limit=maxCaptureResolution();
    if (requested>limit) {
        Serial.printf("[cam] %s exceeds available PSRAM — stepping to %s\n",captureLabel(requested),captureLabel(limit));
        target=frameSize(limit);
    }
    bool initialized=start(target,PIXFORMAT_JPEG,&tuning);
    if (!initialized && target>FRAMESIZE_HD) {
        Serial.println("[cam] capture init failed — retrying at HD");
        stop();
        delay(80);
        initialized=start(FRAMESIZE_HD,PIXFORMAT_JPEG,&tuning);
    }
    if (!initialized) {
        Serial.println("[cam] capture init failed — restoring preview");
        return StillResult::InitFailed;
    }
    // Settle based on the requested mode, preserving the original fallback policy.
    if (requested==CaptureResolution::Vga) {
        const uint32_t started=millis();
        hal::camera::CameraFrame warmup{};
        if (!read(&warmup)) {
            Serial.println("[cam] VGA settle frame unavailable - restoring preview");
            return StillResult::WarmupFailed;
        }
        returnFrame();
        Serial.printf("[cam] VGA capture settle: discarded=1 elapsed_ms=%lu\n",(unsigned long)(millis()-started));
    } else {
        settle(requested==CaptureResolution::FiveMp?1100u:800u);
    }
    return StillResult::Ready;
}
} }
