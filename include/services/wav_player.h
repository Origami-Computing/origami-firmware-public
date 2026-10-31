#pragma once
/**
 * @file services/wav_player.h
 * @brief PCM playback engine for WAV and the firmware's MJPEG/PCM AVI recordings.
 */

#include <stdint.h>
#include "hal/hal_sd.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "services/media_volume.h"
#include "core/pcm_ring.h"
#include "core/playback_progress.h"
#include "core/mjpeg_avi.h"
#include "services/playback_worker.h"

namespace services {
namespace audio {

class WavPlayer {
public:
    ~WavPlayer();
    static constexpr int kVisBars = 16;

    struct TrackInfo {
        int      hz         = 0;
        int      channels   = 0;
        uint32_t durationMs = 0;
        char     title[64]  = {};
    };

    // Own background-I/O reservation, codec preparation and playback worker.
    // A failed open still permits browsing; closeSession releases acquired state.
    bool openSession();
    void closeSession();

    bool ready() const { return !worker_.cleanupPending() && ring_.buf != nullptr && pcmBuf_ != nullptr; }

    // Validate a WAV without starting I2S playback. Used by recording browsers
    // to keep empty or malformed files out of the playable index.
    static bool probeTrack(const char* path);

    // Accepts PCM16 WAV and the firmware's bounded MJPEG/PCM AVI audio stream.
    bool startTrack(const char* path);
    // Terminal embedded-AVI read/demux failure; reset on start/restart.
    bool failed() const { return streamFailed_.load(); }
    // UI-task query: closeSession may retain ownership until worker acknowledgment.
    bool sessionOpen() const { return sessionOpen_; }
    // False means the worker did not yield within 200 ms; track is unchanged.
    bool stopTrack();
    bool restartTrack();

    void pause();
    void resume();
    bool isPlaying() const { return playing_; }
    bool isOpen() const { return fileOpen_; }

    void setVolume(int vol0to100) { services::audio::media_volume::setVolume(vol0to100); }
    int  volume() const { return services::audio::media_volume::volume(); }
    int  scaledVolumeBasisPoints() const;

    uint32_t elapsedMs() const;
    // Audio handed to I2S, excluding file read-ahead. This can lead physical
    // playback by the active DMA buffer duration; not a hardware presentation clock.
    uint32_t outputElapsedMs() const { return outputProgress_.elapsedMs(); }
    bool consumeWantsNextTrack();
    const TrackInfo& trackInfo() const { return info_; }

    // Independent atomic bars, not a coherent multi-bar frame.
    void copySpectrum(uint8_t (&out)[kVisBars]) const {
        for (int i = 0; i < kVisBars; ++i) out[i] = spectrum_[i].load(std::memory_order_relaxed);
    }
    void clearSpectrum() {
        for (auto& bar : spectrum_) bar.store(0, std::memory_order_relaxed);
    }

private:
    bool usageTrackActive_ = false;
    std::atomic<uint8_t> spectrum_[kVisBars]{};
    void stopTrackUnlocked(); // Caller holds PlaybackWorker::Control.
    bool init();
    bool startTask();
    // Bounded 200 ms wait; closeSession defers cleanup on timeout.
    bool stopTask();
    bool sessionOpen_ = false;
    bool sessionReady_ = false;
    bool backgroundIoPaused_ = false;

    struct ParsedHeader {
        int      hz = 0;
        int      channels = 0;
        uint32_t dataStart = 0;
        uint32_t dataEnd = 0;
        uint32_t bytesPerFrame = 0;
        uint32_t durationMs = 0;
        core::avi::Info avi{};
    };

    static bool parseHeader_(hal::sd::StreamReader& file, ParsedHeader& parsed);
    bool recoverSdFile_();
    bool atEndOfFile_();
    bool pumpFileToRing_();
    // Worker/control-only, with SPI held. Bounded scan skips video payloads.
    bool prepareAviAudioChunk_();
    bool failAviStream_();
    void drainToI2S_();
    void analyzeVis_(const int16_t* pcm, int frames, int channels);

    static void taskFn_(void* arg);

    core::dsp::PcmRing ring_{};
    int16_t*           pcmBuf_ = nullptr;
    int                pcmBufSamples_ = 0;

    // StreamReader owns each SPI operation. Existing pump/header batches retain
    // the recursive SPI lock across their bounded sequence of stream calls.
    hal::sd::StreamReader file_{};
    std::atomic<bool> fileOpen_{false};
    char               currentPath_[192] = {};

    uint32_t           dataStart_ = 0;
    uint32_t           dataEnd_ = 0;
    uint32_t           bytesPerFrame_ = 0;
    uint32_t           lastGoodFilePos_ = 0;
    core::audio::PlaybackProgress progress_;
    core::audio::PlaybackProgress outputProgress_;
    core::avi::Info avi_{};
    uint32_t aviChunkEnd_=0, aviSamplesRead_=0;
    std::atomic<bool> streamFailed_{false};
    uint32_t outputSampleRemainder_ = 0; // Stereo output; retain a partial frame.

    std::atomic<bool>  playing_{false};
    std::atomic<bool>  wantsNext_{false};

    TrackInfo          info_{};
    PlaybackWorker worker_;
};

}  // namespace audio
}  // namespace services
