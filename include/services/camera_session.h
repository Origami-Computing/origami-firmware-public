#pragma once
#include "services/agent_coordination.h"
#include "hal/hal_camera.h"
#include "services/camera_capture.h"
#include "services/peripheral_reservation.h"

namespace services { namespace camera {

// UI-task camera lifetime shared by photography and QR scanning. Uses the
// existing HAL driver/retry/frame ownership and Wi-Fi manager policies.
class Session {
public:
    // Exclusive frame access for one recording worker. Acquire/close on the UI
    // task, before spawn/after acknowledged worker exit respectively. The Session
    // must outlive the loan. read/returnFrame belong to that single worker only;
    // read may block for the HAL frame timeout; borrowed DMA bytes expire on returnFrame. No allocation or extra task.
    // While loaned, UI reads/reconfiguration and background release are rejected.
    class FrameReader {
    public:
        FrameReader() = default;
        ~FrameReader();
        FrameReader(const FrameReader&) = delete;
        FrameReader& operator=(const FrameReader&) = delete;
        bool acquire(Session& session);
        bool read(hal::camera::CameraFrame* frame);
        void returnFrame();
        void close();
    private:
        Session* session_ = nullptr;
        bool frameHeld_ = false;
    };
    enum class PrepareResult { Ready, BackgroundBusy, AgentBusy, CameraBusy };
    enum class StillResult { Ready, BackgroundBusy, AgentBusy, InitFailed, WarmupFailed, CameraBusy };
    enum class Resume { Immediate, Deferred, DeferredWithInputRecovery };
    using StageObserver = void (*)(const char*);
    ~Session();
    Session() = default;
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // One UI-task owner until driver stop AND background release. This keeps
    // a draining consumer from racing another session, including radio restore.
    PrepareResult prepareCapture(bool deepPowerDown);
    // Restart for a still photo using existing radio, memory, retry and settle
    // policy. On any result the caller retains this session and must restore a
    // preview or stop/release it. No storage, display or navigation side effects.
    StillResult prepareStill(CaptureResolution requested, const hal::camera::Tuning& tuning);
    // Initial preview after UI surfaces are prepared: wait 100 ms, initialize
    // DMA, prepare the consumer, then settle 700 ms. No warmup frame is discarded.
    // jpegQuality follows HAL semantics (-1 preserves the resolution default).
    // pixelFormat=-1 selects JPEG; explicit HAL formats require a matching
    // consumer. The session retains the same acquisition/cleanup contract.
    // Consumer setup may enable its rendering path before settling; no display
    // ownership enters the service. On failure stop the camera, as for restart.
    bool openPreview(int frameSize, const hal::camera::Tuning& tuning,
                     bool (&prepareConsumer)(), int jpegQuality = -1, int pixelFormat = -1);
    // Restart preview within the current reservation. The caller releases
    // its decoder first; prepareConsumer runs after camera DMA initialization and
    // before settling. On failure stop the camera; the caller releases consumer
    // resources. A missing warmup frame remains non-fatal, as in the existing UI.
    // UI-task only; callbacks must not re-enter this session.
    bool restartPreview(int frameSize, const hal::camera::Tuning& tuning,
                        uint32_t settleMs, bool (&prepareConsumer)(), int jpegQuality = -1, int pixelFormat = -1);
    // QR's existing Wi-Fi manager stops the radios after this reservation.
    PrepareResult prepareScan(StageObserver observer = nullptr);
    void releaseBackground();
    // Photography owns radio restoration; QR delegates it to the Wi-Fi manager.
    void restoreUi(Resume mode);
    bool backgroundHeld() const { return reservation_.held(); }
    bool transportHeld() const { return agentPause_.held(); }

    // Omitted settings select the existing HAL diagnostic defaults.
    bool start(int frameSize = -1, int pixelFormat = -1, const hal::camera::Tuning* tuning = nullptr, int jpegQuality = -1);
    void stop();
    bool running() const;
    void settle(uint32_t milliseconds);
    bool read(hal::camera::CameraFrame* frame);
    void returnFrame();
    bool captureToFile(const char* path);
    // Existing HAL diagnostic snapshot naming/copy policy; no photo upload queue.
    bool captureDiagnosticToSd();
    const char* lastError() const;
private:
    bool claim();
    void releaseIfIdle();
    bool initializePreview(int frameSize, const hal::camera::Tuning& tuning,
                           uint32_t settleMs, bool (&prepareConsumer)(), int jpegQuality, int pixelFormat);
    services::background_network::PeripheralReservation reservation_;
    agent_coordination::TransportPause agentPause_;
    bool ownsCamera_ = false;
    bool inputRecoveryPending_ = false; // Survives radio restore while browsing Camera settings.
    bool frameHeld_ = false; // UI-task borrowing only.
    FrameReader* reader_ = nullptr; // UI-task loan ownership; immutable while worker runs.
};
} }
