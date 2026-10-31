#pragma once
#include "core/mjpeg_avi.h"
#include "core/avi_index.h"
#include "services/peripheral_reservation.h"
#include "services/wav_player.h"
#include <atomic>

namespace services { namespace video {
// UI-task library/session. Owns catalog, peripheral reservation, JPEG prefetch
// worker, PCM playback session and bounded PSRAM storage (six JPEG slots plus
// 4 KiB index-read scratch and a bounded validated frame index). The app owns playback
// choices/rendering; audioPositionMs exposes delivered PCM time for scheduling.
// open scans up to 96 names synchronously, as in the existing gallery; frame
// reads run on VideoRead in at most 8 KiB calls, with cancellation checks
// between calls; PCM uses the existing WavPlay worker. close/stop wait
// cooperatively for both workers before releasing the transport/radio reservation.
class Library {
public:
    enum class State { Idle, Loading, Ready, End, Failed };
    struct Frame { const uint8_t* data=nullptr; uint32_t length=0, number=0, imageNumber=0; };
    ~Library() { close(); }
    Library()=default;
    Library(const Library&)=delete;
    Library& operator=(const Library&)=delete;
    // With deferred=true, false plus opening() means retry on a later UI tick.
    // close cancels pending admission and restores background work.
    bool open(bool deferred=false);
    bool opening() const { return opening_; }
    void close();
    int count() const { return count_; }
    const char* path(int index) const;
    bool select(int index);
    void stop();
    // UI task, after a frame is ready. Silent clips succeed without opening audio.
    // First start can block for codec/file preparation; later starts resume.
    bool startAudio();
    void pauseAudio() { audio_.pause(); }
    bool audioFailed() const { return audioStarted_ && audio_.failed(); }
    uint32_t audioPositionMs() const { return audioStarted_ ? audio_.outputElapsedMs() : 0; }
    State state() const { return state_.load(); }
    // Info valid after Ready/End, and error after Failed (until next selection).
    const core::avi::Info& info() const { return info_; }
    const char* error() const { return error_; }
    // imageNumber identifies unchanged JPEG content within this selection, even
    // across held timeline intervals. A skipped frame can still be decoded.
    // Borrow at most one frame on the UI task; release before stop/select/close.
    // A finite target selects the newest published frame due at that position;
    // future frames stay queued, and the reader may skip expired payloads toward
    // this monotonically advancing target. UINT32_MAX takes the oldest published
    // frame; it cannot undo an earlier skip request.
    bool take(Frame& frame, uint32_t atOrBefore=UINT32_MAX);
    void release();
private:
    static constexpr uint32_t kSlots=6, kFrameBytes=core::avi::kJpegFrameBytes;
    static constexpr size_t kReadScratchBytes=4096;
    static constexpr int kFiles=96, kPath=64;
    static void run(void* self);
    void read();
    background_network::PeripheralReservation reservation_;
    services::audio::WavPlayer audio_;
    bool audioStarted_=false;
    bool opening_=false;
    char* paths_=nullptr;
    uint8_t* buffers_=nullptr;
    core::avi::IndexedFrame* index_=nullptr;
    uint32_t lengths_[kSlots]{},imageNumbers_[kSlots]{},numbers_[kSlots]{},sources_[kSlots]{};
    std::atomic<uint32_t> requested_{0};
    std::atomic<uint32_t> head_{0},tail_{0};
    std::atomic<bool> cancel_{false},running_{false};
    std::atomic<State> state_{State::Idle};
    core::avi::Info info_{};
    int count_=0,selected_=0;
    bool borrowed_=false;
    bool radioStopped_=false;
    const char* error_="";
};
} }
