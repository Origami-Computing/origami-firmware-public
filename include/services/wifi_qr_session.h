#pragma once
#include "services/camera_session.h"
#include "hal/hal_wifi.h"

namespace services { namespace wifi_qr {

// UI-task owner of the QR camera/decoder/radio reservation. Workflow, display
// bypass and navigation stay with the caller. Uses existing HAL Wi-Fi policies.
class Session {
public:
    using Observer = camera::Session::StageObserver;
    enum class Decoder { Waiting, Ready, Failed, RadioFailed };
    enum class Join { Waiting, Accepted, Rejected };
    enum class Frame { Idle, DecoderStopped, Missing, Rendered, Submitted };
    // The hardware/decoder are singletons. Service lifetime outlives app views.
    static Session& instance();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    bool begin(Observer observer);
    Decoder prepareDecoder(Observer observer);
    bool openCamera();
    void stopCapture();
    void fail();
    Join join(const core::wifi_qr::Credentials&, uint32_t startedMs);
    // Error-page cleanup is polled until both producers have stopped.
    bool releaseStopped();
    // Wait up to the existing 3000 ms, then retain ownership until pollCleanup
    // observes camera and decoder stopped. False reports deferred cleanup.
    // Retire the view/sensitive workflow data before resuming background users.
    // Callback is synchronous and must not re-enter this session.
    bool close(void (&retireView)());
    // The consumer runs synchronously on the UI task with a borrowed frame.
    // Returns the frame after rendering and center-crop submission, even when
    // submission is busy or fails. Consumer returns true only when fresh grayscale
    // is available; false skips submission. It must not close/re-enter this session.
    Frame renderFrame(bool (&consumer)(const hal::camera::CameraFrame&));
    // Called by the UI loop even after the app has left. No new worker.
    void pollCleanup();
    bool closing() const { return closing_; }
    uint32_t id() const { return id_; }
    bool backgroundHeld() const { return camera_.backgroundHeld(); }
    bool transportHeld() const { return camera_.transportHeld(); }
    const char* cameraError() const { return camera_.lastError(); }
    // Startup error for the last failed decoder preparation, borrowed until
    // another preparation. Apps do not inspect the decoder worker's lifecycle.
    const char* decoderError() const;
private:
    Session() = default;
    bool closing_ = false;
    void release();
    camera::Session camera_;
    uint32_t id_ = 0;
};
} }
