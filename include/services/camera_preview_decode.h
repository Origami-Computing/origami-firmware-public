#pragma once
#include "core/preview_decoder.h"
#include <atomic>

namespace services { namespace camera {
// Optional camera/video-preview decoder. UI task owns begin/submit/acquire/release/
// close; a Core-0 worker owns decoding. Two slots bound memory and backpressure.
// Inputs are copied synchronously; acquired output stays immutable until release.
// close joins codec work before freeing buffers; caller must release its borrow.
// No sensor, audio, display or network ownership: caller retains media admission.
class PreviewDecode {
public:
    ~PreviewDecode() { close(); }
    // fit=true preserves the full image for playback. The worker yields after
    // each bounded frame and checks cancellation between decoder rows.
    bool begin(bool fit=false);
    bool active() const { return running_.load(); }
    // Latched codec failure until begin; no partial pixels are published.
    bool failed() const { return failed_.load(); }
    // Timestamp is caller-owned source time (sensor DMA or media timeline).
    bool submit(const uint8_t*, size_t, uint64_t sensorUs);
    struct Frame { const uint16_t* pixels; uint64_t sensorUs; uint32_t decodeUs; };
    bool acquire(Frame&);
    void release();
    void close();
private:
    enum State { Free, Writing, Queued, Decoding, Ready, Borrowed };
    struct Slot {
        std::atomic<State> state{Free};
        uint8_t* jpeg=nullptr;
        uint16_t* pixels=nullptr;
        size_t bytes=0;
        uint64_t sensorUs=0;
        uint32_t decodeUs=0;
    } slots_[2];
    core::images::PreviewDecoder decoder_;
    std::atomic<bool> running_{false}, stop_{false};
    std::atomic<bool> failed_{false};
    bool fit_=false;
    int borrowed_=-1;
    static void task(void*);
    static bool shouldAbort(void*);
    void run();
};
} }
