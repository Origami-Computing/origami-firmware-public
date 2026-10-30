#include "services/wav_capture.h"
#include "core/usage_events.h"
#include "core/recording_files.h"
#include "hal/hal_audio.h"

namespace services { namespace audio {

WavCapture::~WavCapture() {
    release();
}

WavCapture::StartResult WavCapture::begin(const char* directory, const char* prefix,
        char* path, size_t capacity, CodecCheck check, bool pauseAgentTransport) {
    if (ownsMic_ || writer_.isOpen()) return StartResult::AlreadyOpen;
    const auto reserved = reservation_.acquire(pauseAgentTransport);
    if (reserved == services::background_network::PeripheralReservation::Result::PeripheralBusy)
        return StartResult::PeripheralBusy;
    if (reserved == services::background_network::PeripheralReservation::Result::AgentBusy)
        return StartResult::AgentBusy;
    hal::audio::hardwareReset();
    const bool codec = hal::audio::initCodec(0x18);
    const bool configured = codec && hal::audio::configureForRecording();
    if ((check == CodecCheck::Required && !configured) || !hal::audio::initMic()) {
        hal::audio::endMicSession();
        reservation_.release();
        return StartResult::MicUnavailable;
    }
    ownsMic_ = true;
    if (!core::recording_files::makePath(directory, prefix, path, capacity) ||
        !writer_.open(path, kSampleRate, 1)) {
        hal::audio::endMicSession();
        ownsMic_ = false;
        reservation_.release();
        return StartResult::FileUnavailable;
    }
    processor_.reset(kSampleRate);
    waveform_.reset();
    samplesWritten_ = 0;
    core::usage::emit("audio","audio.capture");
    return StartResult::Started;
}

bool WavCapture::drain() {
    if (!writer_.isOpen() || !hal::audio::micReady()) return true;
    int32_t raw[256];
    int16_t pcm[256];
    for (int batch = 0; batch < 16; ++batch) {
        size_t bytes = 0;
        if (hal::audio::readMic(raw, sizeof(raw), &bytes, 0) != ESP_OK || !bytes) break;
        const size_t count = bytes / sizeof(int32_t);
        waveform_.ingestRaw(raw, count);
        processor_.processRaw(raw, pcm, count);
        if (!writer_.write(pcm, (int)count)) return false;
        samplesWritten_ += count;
    }
    return true;
}

bool WavCapture::finish(FinishMode mode) {
    const bool wasOpen = writer_.isOpen();
    const bool ok = writer_.close();
    if (wasOpen) core::usage::emit("audio",ok ? "audio.finalized" : "audio.failed");
    if (ownsMic_) {
        if (mode == FinishMode::RestorePlayback) hal::audio::restoreForUiPlayback();
        else hal::audio::deinitMic();
        ownsMic_ = false;
    }
    return ok;
}

void WavCapture::release() {
    if (ownsMic_ || writer_.isOpen()) finish();
    reservation_.release();
}

void WavCapture::close() {
    release();
    hal::audio::restoreForUiPlayback();
}

} }
