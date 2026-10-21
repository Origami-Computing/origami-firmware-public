#pragma once
#include <atomic>
#include <stdint.h>

namespace core { namespace audio {

// Frames are updated by one worker or its excluded UI control. Readers sample
// only the published elapsed milliseconds, never the non-atomic 64-bit counter.
// Keep the players' integer rounding and uint32_t elapsed wrap behavior.
class PlaybackProgress {
public:
    void reset() { frames_ = 0; elapsed_.store(0, std::memory_order_relaxed); }
    void add(uint32_t frames, int rate) { frames_ += frames; publish(rate); }
    void seek(uint32_t milliseconds, int rate) {
        frames_ = rate > 0 ? (uint64_t)milliseconds * (uint64_t)rate / 1000u : 0;
        publish(rate);
    }
    uint32_t elapsedMs() const { return elapsed_.load(std::memory_order_relaxed); }
private:
    void publish(int rate) {
        elapsed_.store(rate > 0 ? (uint32_t)(frames_ * 1000ULL / (uint64_t)rate) : 0,
                       std::memory_order_relaxed);
    }
    uint64_t frames_ = 0;
    std::atomic<uint32_t> elapsed_{0};
};

} }
