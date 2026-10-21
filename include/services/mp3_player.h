#pragma once
/**
 * @file services/mp3_player.h
 * @brief Self-contained MP3 playback engine.
 *
 * Owns: minimp3 decoder, PSRAM input + PCM ring buffers, SD file handle,
 * I2S drain, FreeRTOS decode task, ID3v2 tag parsing, spectrum analysis.
 *
 * The app retains: playlist (file list), browse index, canvas sub-state
 * toggle, vis display heights, and all draw/input logic. Media volume is owned
 * by services::audio::media_volume and shared with WAV playback.
 *
 * Threading model (unchanged from the original app):
 *   Decode task (core 0, pri 5): decodeOneFrame_() + drainToI2S_()
 *   Main loop  (core 1, pri 1): startTrack / stopTrack / seek / pause
 * Decode and drain share one worker; PcmRing itself has no synchronization.
 * Main-loop track mutations acquire a bounded Control guard between iterations.
 * Atomic pause/EOF flags do not imply that an in-flight iteration has finished.
 * Elapsed time and file-open state are atomically published for UI reads.
 */

#include <stddef.h>
#include <stdint.h>
#include <FS.h>

#include "core/audio_dsp.h"
#include "core/mp3_codec.h"
#include "services/media_volume.h"
#include "core/pcm_ring.h"
#include "core/playback_progress.h"
#include "services/playback_worker.h"
#include "minimp3.h"

namespace services {
namespace audio {

class Mp3Player {
public:
    ~Mp3Player();

    struct TrackInfo {
        int      hz          = 0;
        int      channels    = 0;
        uint32_t bitrateKbps = 0;
        uint32_t durationMs  = 0;
        char     title[64]   = {};
        char     artist[64]  = {};
    };

    // Own codec preparation and decoder-worker lifetime for one Music visit.
    // Repeated opens retain allocated buffers, matching existing app re-entry.
    // UI-task only. False means upload readiness, buffers, codec or worker
    // were unavailable. A partial session must still be closed. Repeated close
    // is harmless. Existing Music failure policy is preserved by the caller.
    bool openSession();
    void closeSession();

    // Low-level diagnostic hooks; application code uses openSession/closeSession.
    // Retain existing buffers. New rings try all supported PSRAM sizes before
    // internal fallback; input prefers PSRAM at a fixed size. False if missing.
    bool init();
    bool ready() const { return !worker_.cleanupPending() && inBuf_ != nullptr && ring_.buf != nullptr; }

    // Open a file and begin playback: parse ID3, probe format, init I2S,
    // prebuffer ~20 frames.  Sets trackInfo() on success.
    // Returns false if the file cannot be opened or the format is unrecognised.
    // Does NOT touch canvas pipeline — caller does mp3SetSubState after return.
    bool startTrack(const char* path);

    // False means the worker did not yield within 200 ms; track is unchanged.
    bool stopTrack();

    // Byte-accurate seek: recalculates target offset from elapsed+seconds,
    // syncs past garbage bytes, and refills the ring.
    void seekRelativeSeconds(int seconds);

    void setVolume(int vol0to100) { services::audio::media_volume::setVolume(vol0to100); }
    int  volume()                 const { return services::audio::media_volume::volume(); }
    int  scaledVolume() const;
    int  scaledVolumeBasisPoints() const;

    // pause/resume only toggle the playing flag; the task loop uses it.
    void pause()  { playing_ = false; }
    void resume() { if (worker_.running() && !worker_.cleanupPending() && fileOpen_) playing_ = true; }
    bool isPlaying() const { return playing_; }
    bool isOpen()    const { return fileOpen_; }

    uint32_t elapsedMs() const;

    // Returns true (and clears the flag) when the decode task signals EOF.
    // Only the main task calls this.
    bool consumeWantsNextTrack();

    const TrackInfo& trackInfo() const { return info_; }

    // Low-level diagnostic worker hooks. Session methods own these for apps.
    // stopTask() requests shutdown and waits up to 200 ms; timeout is not an
    // acknowledgment. closeSession retains resources and schedules cleanup on timeout.
    /** Start the decoder task; false on allocation failure or pending stop. */
    bool startTask();
    bool stopTask();

private:
    void stopTrackUnlocked(); // Caller holds PlaybackWorker::Control.
    bool sessionOpen_ = false;
    bool sessionReady_ = false;
    core::audio::Mp3Codec           codec_;
    core::dsp::PcmRing ring_{};
    uint8_t*           inBuf_   = nullptr;
    int16_t            decodeBuf_[MINIMP3_MAX_SAMPLES_PER_FRAME];

    fs::File           file_;
    std::atomic<bool> fileOpen_{false};
    uint32_t           fileSize_ = 0;
    uint32_t           lastGoodFilePos_ = 0;
    char               currentPath_[192] = {};

    std::atomic<bool>  playing_{false};

    int                detectedRate_   = 0;
    uint32_t           estBitrate_     = 0;
    core::audio::PlaybackProgress progress_;
    uint8_t            decodeNoProgressCount_ = 0;

    TrackInfo          info_{};

    PlaybackWorker worker_;
    std::atomic<bool>  wantsNext_{false};

    // Called only from the decode task.
    int  decodeOneFrame_();
    void drainToI2S_();
    void parseID3_();
    /** True when the SD file is exhausted and the codec buffer has no full frame left. */
    bool atEndOfCompressedStream_();
    bool recoverSdFile_();

    static void taskFn_(void* arg);
};

}  // namespace audio
}  // namespace services
