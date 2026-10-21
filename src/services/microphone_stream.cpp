#include "services/microphone_stream.h"
#include "hal/hal_audio.h"

namespace services { namespace audio {
bool MicrophoneStream::open(Sink sink, void* user) {
    close();
    capture_ = services::audio::MicCapture{};
    if (!capture_.startStream(sink, user)) {
        // Preserve the meter/game failed-start rollback and later UI restore.
        hal::audio::endMicSession();
        state_ = State::Failed;
        return false;
    }
    state_ = State::Open;
    return true;
}
void MicrophoneStream::poll() {
    if (state_ == State::Open) capture_.tick();
}
void MicrophoneStream::close() {
    if (state_ == State::Open) capture_.stop();
    else if (state_ == State::Failed) hal::audio::restoreForUiPlayback();
    state_ = State::Idle;
}
} }
