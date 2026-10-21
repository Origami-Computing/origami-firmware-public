#include "services/diagnostic_recording.h"
#include "hal/hal_audio.h"

namespace services { namespace diagnostics {
RawRecording::~RawRecording() {
    if (writer_.isOpen()) (void)finish();
}
RawRecording::StartResult RawRecording::begin(const char* path) {
    hal::audio::txEnd();
    if (!hal::audio::initMic()) {
        hal::audio::restoreForUiPlayback();
        return StartResult::MicUnavailable;
    }
    if (!writer_.open(path, 48000, 1)) {
        hal::audio::restoreForUiPlayback();
        return StartResult::FileUnavailable;
    }
    samples_ = 0;
    return StartResult::Started;
}
void RawRecording::drain() {
    int32_t raw[128]; size_t bytesRead = 0;
    if (hal::audio::readMic(raw, sizeof(raw), &bytesRead, 0) == ESP_OK && bytesRead > 0) {
        const int n = (int)(bytesRead / sizeof(int32_t));
        int16_t pcm[128];
        for (int i = 0; i < n; ++i) pcm[i] = (int16_t)(raw[i] >> 16);
        (void)writer_.write(pcm, n);
        samples_ += (uint32_t)n;
    }
}
bool RawRecording::finish() {
    const bool saved = writer_.close();
    hal::audio::restoreForUiPlayback();
    return saved;
}
void RawRecording::restoreUi() {
    if (writer_.isOpen()) (void)writer_.close();
    hal::audio::restoreForUiPlayback();
}
} }
