#pragma once
/**
 * @file services/wav_player.h
 * @brief Lightweight PCM WAV playback engine for recorded clips.
 */

#include <stdint.h>
#include <FS.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "services/media_volume.h"
#include "core/pcm_ring.h"
#include "core/playback_progress.h"
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

    bool startTrack(const char* path);
    // False means the worker did not yield within 200 ms; track is unchanged.
    bool stopTrack();
    bool restartTrack();

    void pause()  { playing_ = false; }
    void resume() { if (worker_.running() && !worker_.cleanupPending() && fileOpen_) playing_ = true; }
    bool isPlaying() const { return playing_; }
    bool isOpen() const { return fileOpen_; }

    void setVolume(int vol0to100) { services::audio::media_volume::setVolume(vol0to100); }
    int  volume() const { return services::audio::media_volume::volume(); }
    int  scaledVolumeBasisPoints() const;

    uint32_t elapsedMs() const;
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
    };

    static bool parseHeader_(fs::File& file, ParsedHeader& parsed);
    bool recoverSdFile_();
    bool atEndOfFile_();
    bool pumpFileToRing_();
    void drainToI2S_();
    void analyzeVis_(const int16_t* pcm, int frames, int channels);

    static void taskFn_(void* arg);

    core::dsp::PcmRing ring_{};
    int16_t*           pcmBuf_ = nullptr;
    int                pcmBufSamples_ = 0;

    fs::File           file_;
    std::atomic<bool> fileOpen_{false};
    char               currentPath_[192] = {};

    uint32_t           dataStart_ = 0;
    uint32_t           dataEnd_ = 0;
    uint32_t           bytesPerFrame_ = 0;
    uint32_t           lastGoodFilePos_ = 0;
    core::audio::PlaybackProgress progress_;

    std::atomic<bool>  playing_{false};
    std::atomic<bool>  wantsNext_{false};

    TrackInfo          info_{};
    PlaybackWorker worker_;
};

}  // namespace audio
}  // namespace services
