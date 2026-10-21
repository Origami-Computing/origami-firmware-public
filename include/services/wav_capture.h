#pragma once
#include "core/wav_writer.h"
#include "services/peripheral_reservation.h"
#include "core/mic_tuning.h"
#include "core/recording_waveform.h"

namespace services { namespace audio {

// UI-task microphone-to-WAV operation. Owns codec/RX, DSP state and the writer.
// Holds background I/O until release(), so callers can finalize and enqueue
// before workers resume. No rendering, navigation or upload is performed.
class WavCapture {
public:
    enum class CodecCheck { Required, BestEffort };
    enum class StartResult { Started, MicUnavailable, FileUnavailable, AlreadyOpen, PeripheralBusy, AgentBusy };
    enum class FinishMode { RestorePlayback, ReleaseMicrophone };
    static constexpr uint32_t kSampleRate = 48000;

    ~WavCapture();
    WavCapture() = default;
    WavCapture(const WavCapture&) = delete;
    WavCapture& operator=(const WavCapture&) = delete;

    // BestEffort preserves Recorder's existing codec-error policy; reflection
    // uses Required. Any failed start releases the microphone session.
    StartResult begin(const char* directory, const char* prefix, char* path, size_t capacity,
                      CodecCheck check = CodecCheck::Required,
                      bool pauseAgentTransport = false);
    // Drain at most 16 DMA blocks without waiting for new samples. A false
    // result means SD write failure; caller must finish and decide file policy.
    bool drain();
    // Finalize/verify WAV and release RX. Result must be considered by callers
    // that promise a saved recording. Meter/sample history remains readable.
    bool finish(FinishMode mode = FinishMode::RestorePlayback);
    // End the processing reservation after save/discard/queue decisions.
    void release();
    // Leave the app session and restore the UI audio path.
    void close();
    bool isOpen() const { return writer_.isOpen(); }
    uint64_t samples() const { return processor_.samples(); }
    uint32_t samplesWritten() const { return samplesWritten_; }
    const core::audio::RecordingWaveform& waveform() const { return waveform_; }
    void resetWaveform() { waveform_.reset(); }

private:
    services::background_network::PeripheralReservation reservation_;
    core::WavWriter writer_;
    core::audio::mic_tuning::Processor processor_;
    core::audio::RecordingWaveform waveform_;
    uint32_t samplesWritten_ = 0;
    bool ownsMic_ = false;
};

} }
