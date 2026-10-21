#pragma once
#include "core/wav_writer.h"

namespace services { namespace diagnostics {
// UI-task raw microphone check: mono 48 kHz PCM, no DSP or background worker.
// Caller supplies a destination and controls hold/release; this operation owns
// RX setup, one-block draining, writer state and playback restoration.
class RawRecording {
public:
    enum class StartResult { Started, MicUnavailable, FileUnavailable };
    RawRecording() = default;
    RawRecording(const RawRecording&) = delete;
    RawRecording& operator=(const RawRecording&) = delete;
    ~RawRecording();
    StartResult begin(const char* path);
    // One non-blocking read of up to 128 int32 samples, shifted by 16. The
    // legacy sample count includes attempted writes even if storage fails.
    void drain();
    bool finish();
    // App exit also restores playback when no recording was started.
    void restoreUi();
    bool active() const { return writer_.isOpen(); }
    uint32_t samples() const { return samples_; }
private:
    core::WavWriter writer_;
    uint32_t samples_ = 0;
};
} }
