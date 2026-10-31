#include "services/camera_session.h"
#include "services/agent_coordination.h"
#include "hal/hal_wifi.h"
#include "hal/hal_io.h"
#include "Pins.h"
#include <Wire.h>
#include <assert.h>

namespace services { namespace camera {
namespace { Session* owner = nullptr; } // UI-task only, like all Session calls.
bool Session::claim() {
    if (reader_ || (owner && owner != this)) return false;
    owner = this;
    return true;
}
void Session::releaseIfIdle() {
    if (owner == this && !ownsCamera_ && !reservation_.held()) owner = nullptr;
}

Session::~Session() {
    assert(!reader_); // Worker must be joined and its loan closed first.
    stop();
    releaseBackground();
}

Session::PrepareResult Session::prepareScan(StageObserver observer) {
    if (!claim()) return PrepareResult::CameraBusy;
    if (observer) observer("before-pause");
    if (reservation_.acquire(false, 8000) != background_network::PeripheralReservation::Result::Acquired) {
        releaseIfIdle();
        return PrepareResult::BackgroundBusy;
    }
    if (observer) observer("io-paused");
    if (!agentPause_.acquire(8000)) {
        releaseBackground();
        return PrepareResult::AgentBusy;
    }
    if (observer) observer("agent-paused");
    return PrepareResult::Ready;
}

Session::PrepareResult Session::prepareCapture(bool deepPowerDown) {
    const auto result = prepareScan();
    if (result != PrepareResult::Ready) return result;
    if (deepPowerDown) hal::wifi::powerDown();
    else hal::wifi::stop();
    agentPause_.release();
    return PrepareResult::Ready;
}

void Session::releaseBackground() {
    if (reader_) return;
    // Scanner ordering: release transport before restarting network workers.
    agentPause_.release();
    reservation_.release();
    releaseIfIdle();
}

void Session::restoreUi(Resume mode) {
    // A rejected/unstarted caller owns no radio or SCCB transition to undo.
    // In particular, failed admission must not disrupt an OTA metadata request.
    if (reader_ || (owner && owner != this)) return;
    if (owner == this) {
        if (mode == Resume::Immediate) hal::wifi::restore();
        else hal::wifi::scheduleRestore();
        releaseBackground();
    }
    if (mode == Resume::DeferredWithInputRecovery && inputRecoveryPending_) {
        // Preserve the existing rev-4 recovery after camera SCCB/I2C use.
        inputRecoveryPending_ = false;
        Wire.end();
        Wire.begin(SDA_PIN, SCL_PIN);
        (void)hal::io::reinitAfterI2CRestart();
    }
}

bool Session::start(int frameSize, int pixelFormat, const hal::camera::Tuning* tuning, int jpegQuality) {
    // Even a failed attempt may have configured pins/rails. Keep the session
    // responsible for cleanup, including retry after a HAL I2C-lock timeout.
    if (!claim()) return false;
    ownsCamera_ = true;
    inputRecoveryPending_ = true;
    return hal::camera::init(frameSize, pixelFormat, tuning, jpegQuality);
}

void Session::stop() {
    if (reader_ || !ownsCamera_) return;
    hal::camera::deinit();
    ownsCamera_ = hal::camera::initialized();
    if (!ownsCamera_) frameHeld_ = false;
    releaseIfIdle();
}

bool Session::running() const { return ownsCamera_ && hal::camera::initialized(); }
void Session::settle(uint32_t milliseconds) { if (!reader_ && running()) hal::camera::settleAutoExposure(milliseconds); }
bool Session::read(hal::camera::CameraFrame* frame) { if (reader_ || frameHeld_ || !running()) return false;
    return frameHeld_ = hal::camera::getFrame(frame); }
void Session::returnFrame() { if (!reader_ && ownsCamera_) {hal::camera::returnFrame();frameHeld_ = false;} }
bool Session::captureToFile(const char* path) { return !reader_ && !frameHeld_ && running() && hal::camera::captureToFile(path); }
bool Session::captureDiagnosticToSd() { return !reader_ && !frameHeld_ && running() && hal::camera::captureToSd(); }
const char* Session::lastError() const {
    if (reader_) return "Camera reader busy";
    return owner && owner != this ? "Camera session busy" : hal::camera::lastInitError();
}
Session::FrameReader::~FrameReader() { close(); }
bool Session::FrameReader::acquire(Session& session) {
    if (session_ || session.reader_ || session.frameHeld_ ||
        !session.running() || !session.backgroundHeld()) return false;
    session.reader_ = this;
    session_ = &session;
    return true;
}
bool Session::FrameReader::read(hal::camera::CameraFrame* frame) {
    if (!session_ || frameHeld_) return false;
    return frameHeld_ = hal::camera::getFrame(frame);
}
void Session::FrameReader::returnFrame() {
    if (frameHeld_) {hal::camera::returnFrame();frameHeld_ = false;}
}
void Session::FrameReader::close() {
    if (!session_) return;
    // Only after the worker has stopped; never revoke an in-flight DMA borrow.
    returnFrame();
    session_->reader_ = nullptr;
    session_ = nullptr;
}
} }
