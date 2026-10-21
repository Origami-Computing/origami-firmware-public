#include "services/camera_session.h"
#include "services/agent_coordination.h"
#include "hal/hal_wifi.h"
#include "hal/hal_io.h"
#include "Pins.h"
#include <Wire.h>

namespace services { namespace camera {
namespace { Session* owner = nullptr; } // UI-task only, like all Session calls.
bool Session::claim() {
    if (owner && owner != this) return false;
    owner = this;
    return true;
}
void Session::releaseIfIdle() {
    if (owner == this && !ownsCamera_ && !reservation_.held()) owner = nullptr;
}

Session::~Session() {
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
    // Scanner ordering: release transport before restarting network workers.
    agentPause_.release();
    reservation_.release();
    releaseIfIdle();
}

void Session::restoreUi(Resume mode) {
    // A rejected caller must not restore radios or reset SCCB beneath the owner.
    if (owner && owner != this) return;

    if (mode == Resume::Immediate) hal::wifi::restore();
    else hal::wifi::scheduleRestore();
    releaseBackground();
    if (mode == Resume::DeferredWithInputRecovery) {
        // Preserve the existing rev-4 recovery after camera SCCB/I2C use.
        Wire.end();
        Wire.begin(SDA_PIN, SCL_PIN);
        (void)hal::io::reinitAfterI2CRestart();
    }
}

bool Session::start(int frameSize, int pixelFormat, const hal::camera::Tuning* tuning) {
    // Even a failed attempt may have configured pins/rails. Keep the session
    // responsible for cleanup, including retry after a HAL I2C-lock timeout.
    if (!claim()) return false;
    ownsCamera_ = true;
    return hal::camera::init(frameSize, pixelFormat, tuning);
}

void Session::stop() {
    if (!ownsCamera_) return;
    hal::camera::deinit();
    ownsCamera_ = hal::camera::initialized();
    releaseIfIdle();
}

bool Session::running() const { return ownsCamera_ && hal::camera::initialized(); }
void Session::settle(uint32_t milliseconds) { if (running()) hal::camera::settleAutoExposure(milliseconds); }
bool Session::read(hal::camera::CameraFrame* frame) { return running() && hal::camera::getFrame(frame); }
void Session::returnFrame() { if (ownsCamera_) hal::camera::returnFrame(); }
bool Session::captureToFile(const char* path) { return running() && hal::camera::captureToFile(path); }
bool Session::captureDiagnosticToSd() { return running() && hal::camera::captureToSd(); }
const char* Session::lastError() const {
    return owner && owner != this ? "Camera session busy" : hal::camera::lastInitError();
}
} }
