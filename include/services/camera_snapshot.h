#pragma once
#include <stddef.h>
#include <stdint.h>
namespace services { namespace camera {
class Session;
// UI-task owned JPEG copy. Driver frames are returned before capture completes.
// Data remains valid until clear(), the next capture(), or destruction.
class Snapshot {
public:
    enum class Result { Ready, FrameUnavailable, OutOfMemory };
    struct Timing { uint64_t readyUs=0, copiedUs=0, dmaUs=0; unsigned stale=0; };
    Snapshot() = default;
    ~Snapshot();
    Snapshot(const Snapshot&) = delete;
    Snapshot& operator=(const Snapshot&) = delete;
    Result captureVga(Session& session, uint64_t requestUs);
    void clear();
    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
    const Timing& timing() const { return timing_; }
private:
    uint8_t* data_ = nullptr;
    size_t size_ = 0;
    Timing timing_;
};
} }
