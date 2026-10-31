#pragma once
#include <atomic>
#include "core/mjpeg_avi.h"
#include "services/camera_session.h"
#include "core/mic_tuning.h"
#include "services/agent_coordination.h"
#include <stddef.h>
#include <stdint.h>

namespace services { namespace camera {
// UI-task or owned camera-worker JPEG producer, optional microphone RX, and one
// SD writer. The app retains Camera Session admission
// until close() acknowledges the worker. submit copies borrowed JPEGs without
// SD I/O or waiting. stop is asynchronous; close drains/finalizes cooperatively.
// Storage failures retain .part files; a camera timeout may retain a finalized
// clip but reports failure. This service does not enqueue uploads.
class VideoRecording {
public:
    enum class State { Idle, Starting, Recording, Finishing, Saved, Failed };
    static constexpr uint32_t kSingleFrameDurationMs = 33;
    static constexpr uint32_t kMaxDurationMs = 300000;
    ~VideoRecording();
    VideoRecording() = default;
    VideoRecording(const VideoRecording&) = delete;
    VideoRecording& operator=(const VideoRecording&) = delete;
    bool begin(uint16_t width, uint16_t height);
    // UI task, borrowing an active camera reservation until close(). Owns mic
    // admission, a bounded PCM ring and RX worker; writes combined MJPEG/PCM.
    // May block for admission/driver setup. No audio-sized recording allocation.
    bool beginWithAudio(const Session& camera, uint16_t width, uint16_t height);
    // UI task: owns exclusive camera acquisition until close(), including on
    // asynchronous failure. Session must outlive this recorder. No UI submit
    // while attached. Latest preview bytes are immutable until releasePreview.
    // preview=false avoids preview storage/copy work; readPreview returns false.
    bool beginCamera(Session& camera, uint16_t width, uint16_t height, bool audio, bool preview=true);
    bool readPreview(hal::camera::CameraFrame* frame);
    void releasePreview();
    // Copies a single JPEG envelope into the bounded writer queue. Rejects
    // joined images, trailing bytes and incomplete framing as invalid drops;
    // entropy/codec validation is not provided by this admission check.
    bool submit(const uint8_t* jpeg, size_t length, uint32_t now);
    void stop();
    void close();
    State state() const { const auto s=state_.load(); return s==State::Saved && captureFailed_.load() ? State::Failed : s; }
    bool busy() const;
    uint32_t frames() const { return frames_.load(); }
    // Producer diagnostics, retained after close until the next begin.
    // Returns zero while the camera worker runs; manual UI submission is UI-only.
    // Calls rejected outside Recording/stopping are not frame drops.
    struct SubmissionStats {
        uint32_t accepted=0, queueFull=0, oversized=0, invalid=0, maxQueued=0;
    };
    SubmissionStats submissionStats() const { return captureRunning_.load() ? SubmissionStats{} : submissions_; }
    uint32_t dropped() const { const auto s=submissionStats(); return s.queueFull+s.oversized+s.invalid; }
    // UI task: readable after Saved/Failed or close; returns zero while busy.
    // Successfully written combined AVI chunks only. Payload bytes exclude
    // headers/index/padding;
    // write time includes SD lock waits and scheduling, not just card transfer.
    struct WriterStats {
        uint32_t jpegChunks=0, jpegBytes=0, repeatedJpegChunks=0, repeatedJpegBytes=0;
        uint32_t pcmBytes=0, chunkWriteMs=0, maxChunkWriteMs=0;
    };
    WriterStats writerStats() const { return busy() ? WriterStats{} : writerStats_; }
    bool hasAudio() const { return withAudio_; }
    uint32_t audioSamples() const { return audioWritten_.load(); }
    uint32_t repeatedFrames() const { return repeatedFrames_.load(); }
    // Valid only after Saved/Failed; remains valid until next begin().
    const char* path() const { return path_; }
    const char* error() const { return captureFailed_.load() ? "Camera frame timeout" : error_; }
    struct CaptureStats {
        uint32_t reads=0, unique=0, maxReadUs=0, maxBytes=0;
        uint64_t readUs=0, firstSensorUs=0, lastSensorUs=0;
    };
    // Completed capture measurements only; no torn cross-task snapshots.
    CaptureStats captureStats() const { return captureRunning_.load() ? CaptureStats{} : captureStats_; }
private:
    bool submitFrame(const uint8_t*, size_t, uint32_t);
    static void runCamera(void*);
    void captureCamera();
    void closeCamera();
    Session::FrameReader cameraReader_;
    bool cameraAttached_=false; // UI-task only; selects producer ownership.
    std::atomic<bool> captureRunning_{false}, captureFailed_{false};
    CaptureStats captureStats_{};
    uint8_t* previewBuffers_=nullptr;
    hal::camera::CameraFrame previewFrames_[3]{};
    std::atomic<bool> previewUsed_[3]{};
    std::atomic<int> previewLatest_{-1};
    int previewBorrowed_=-1; // UI-task only.
    // Six frames cover about 200 ms at 30 Hz, including the slot being written.
    // Device writes have stalled for 115 ms despite average spare bandwidth.
    // Bounded 768 KiB PSRAM; sustained overload still rejects submissions.
    static constexpr uint32_t kSlots = 6, kFrameBytes = core::avi::kJpegFrameBytes, kMaxFrames = core::avi::kMaxFrames;
    struct Slot { uint32_t length, at; } slots_[kSlots]{};
    static void run(void* self);
    void write();
    bool beginCommon(uint16_t width, uint16_t height, bool withAudio);
    bool prepareAudio();
    void releaseAudio();
    static void runMic(void* self);
    void captureAudio();
    void writeAudio();
    void releaseBuffers();
    std::atomic<State> state_{State::Idle};
    std::atomic<bool> stopping_{false};
    std::atomic<uint32_t> head_{0}, tail_{0}, frames_{0};
    uint8_t* buffers_ = nullptr;
    uint8_t* index_ = nullptr;
    uint16_t width_ = 0, height_ = 0;
    SubmissionStats submissions_{};
    WriterStats writerStats_{};
    char path_[48]{};
    const char* error_ = "";
    static constexpr uint32_t kAudioCapacity=48000, kTimelineFps=30;
    enum class MicFailure { None, Read, Overrun };
    agent_coordination::TransportPause audioPause_;
    core::audio::mic_tuning::Settings micSettings_{};
    int16_t* audioRing_=nullptr;
    uint8_t* lastFrame_=nullptr;
    bool withAudio_=false,ownsMic_=false;
    std::atomic<uint32_t> audioHead_{0},audioTail_{0},audioOriginMs_{0};
    std::atomic<uint32_t> audioWritten_{0},repeatedFrames_{0};
    std::atomic<bool> micRunning_{false},micStarted_{false};
    std::atomic<MicFailure> micFailure_{MicFailure::None};
};
} }
