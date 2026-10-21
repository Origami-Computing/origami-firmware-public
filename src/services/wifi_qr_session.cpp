#include "services/wifi_qr_session.h"
#include "services/qr_decode.h"
#include "core/telemetry.h"
#include <Arduino.h>
#include <esp_camera.h>

namespace services { namespace wifi_qr {
static hal::camera::Tuning s_tuning = [] {
  hal::camera::Tuning t{};
  // Max sharpness for module edges. quirc binarises a thresholded gray
  // buffer, so any softening at module boundaries directly costs detection
  // reliability.
  t.sharpness = 3;
  // Use the HAL's module-level orientation defaults so QR preview/decoding
  // matches the Camera app and saved photos.
  t.autofocus    = true;
  t.singleShotAf = true;
  // ── Exposure tuning for QR capture ────────────────────────────────────────
  // Two failure modes seen in /qr_debug snapshots:
  //   * scan0008/9: motion blur — AEC was holding the shutter open at ~1/30 s
  //     in normal indoor light, so any hand wobble smears the modules.
  //   * scan0012:   blown highlights — phone screens are bright point sources
  //     and the default AEC target overexposes them, washing the QR's white
  //     modules into a single saturated blob with no edges for quirc.
  // Keep AEC at neutral. An earlier experiment dropped the target ~1 EV
  // (ae_level=-1) to tame bright phone-screen QRs, but on some camera modules
  // that bias — combined with a too-short AEC settle — left the whole frame
  // near-black (serial [qr] histograms showed max≈9, mean≈2, caps=0/0), so the
  // scanner could never find finder patterns. Neutral AE lets each device's
  // sensor expose the QR correctly; the longer settle in openCamera() gives
  // AEC time to converge before we start submitting frames to quirc.
  t.ae_level        = 0;
  t.gainceiling_idx = 1;  // GAINCEILING_4X
  return t;
}();

Session& Session::instance() {
    static Session session;
    return session;
}
bool Session::begin(Observer observer) {
    pollCleanup();
    if (closing_) return false;

    if (camera_.prepareScan(observer) != camera::Session::PrepareResult::Ready) return false;
    id_ = hal::wifi::requestQrScan();
    return true;
}
Session::Decoder Session::prepareDecoder(Observer observer) {
    const auto status = hal::wifi::qrStatus();
    if (status.id != id_) return Decoder::Waiting;
    if (status.phase == hal::wifi::QrPhase::Failed) return Decoder::RadioFailed;
    if (status.phase != hal::wifi::QrPhase::Scanning ||
        !services::qr::shutdownWorker(0)) return Decoder::Waiting;
    if (observer) observer("radio-stopped-before-worker");
    if (!services::qr::ensureWorker()) {
        if (observer) observer("worker-failed");
        return Decoder::Failed;
    }
    if (observer) observer("worker-ready-before-camera");
    return Decoder::Ready;
}
bool Session::openCamera() {
    if (!camera_.start(FRAMESIZE_VGA, PIXFORMAT_JPEG, &s_tuning)) {
        LOGE("camera-qr", "scanner camera init failed");
        return false;
    }
    // Preserve the OV5640 exposure convergence interval before the first frame.
    camera_.settle(1000);
    return true;
}
const char* Session::decoderError() const {
    return services::qr::workerStartError();
}
void Session::stopCapture() {
    if (camera_.running()) { camera_.stop(); delay(80); }
}
void Session::fail() {
    stopCapture();
    services::qr::logDiagnostics();
    (void)services::qr::shutdownWorker(0);
}
Session::Join Session::join(const core::wifi_qr::Credentials& credentials, uint32_t startedMs) {
    if (!services::qr::shutdownWorker(0)) return Join::Waiting;
    return hal::wifi::requestQrJoin(id_, credentials, startedMs) ? Join::Accepted : Join::Rejected;
}
void Session::release() {
    if (id_) hal::wifi::releaseQrSession(id_);
    id_ = 0;
    camera_.releaseBackground();
    closing_ = false;
}
bool Session::releaseStopped() {
    stopCapture();
    if (camera_.running() || !services::qr::shutdownWorker(0)) return false;
    release();
    return true;
}
bool Session::close(void (&retireView)()) {
    stopCapture();
    services::qr::logDiagnostics();
    const bool stopped = services::qr::shutdownWorker();
    retireView();
    closing_ = camera_.running() || !stopped;
    if (!closing_) release();
    else LOGW("camera-qr", "cleanup deferred until camera and decoder stop");
    return !closing_;
}
void Session::pollCleanup() {
    if (closing_) (void)releaseStopped();
}
Session::Frame Session::renderFrame(bool (&consumer)(const hal::camera::CameraFrame&)) {
    if (!camera_.running()) return Frame::Idle;
    if (!services::qr::workerReady()) return Frame::DecoderStopped;
    hal::camera::CameraFrame frame;
    if (!camera_.read(&frame)) return Frame::Missing;
    const bool grayReady = consumer(frame);
    const bool submitted = grayReady && !services::qr::scanBusy() && services::qr::submitForScanCenterCrop(320, 240);
    camera_.returnFrame();
    return submitted ? Frame::Submitted : Frame::Rendered;
}
} }
