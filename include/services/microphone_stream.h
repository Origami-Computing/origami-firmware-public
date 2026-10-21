#pragma once
#include "services/mic_capture.h"

namespace services { namespace audio {
// UI-task microphone stream for live consumers. Owns codec/I2S teardown and
// capture state; the consumer owns its sample interpretation. Uses the existing
// 48 kHz capture -> 16 kHz mono conversion and HAL policy without a new worker.
class MicrophoneStream {
public:
    using Sink = services::audio::MicCapture::SampleSink;
    ~MicrophoneStream() { close(); }
    MicrophoneStream() = default;
    MicrophoneStream(const MicrophoneStream&) = delete;
    MicrophoneStream& operator=(const MicrophoneStream&) = delete;
    bool open(Sink sink, void* user=nullptr); // Reopening starts a fresh session.
    void poll();
    void close(); // Idempotent, including after a failed open.
    bool isOpen() const { return state_ == State::Open; }
private:
    enum class State { Idle, Open, Failed };
    State state_ = State::Idle;
    services::audio::MicCapture capture_;
};
} }
