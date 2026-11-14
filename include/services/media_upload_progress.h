#pragma once
#include <atomic>
#include <stdint.h>

namespace services { namespace media_upload {
// The upload worker is the sole writer. Publish a coherent pair to the gallery.
struct Bytes { uint32_t sent, total; };
class Progress {
public:
    void set(uint32_t sent, uint32_t total) {
        sequence_.fetch_add(1);
        sent_.store(sent);
        total_.store(total);
        sequence_.fetch_add(1);
    }
    Bytes snapshot() const {
        for (unsigned attempt=0; attempt<3; ++attempt) {
            const auto before = sequence_.load();
            const Bytes bytes{sent_.load(), total_.load()};
            if (!(before & 1) && before == sequence_.load()) return bytes;
        }
        return {};
    }
private:
    std::atomic<uint32_t> sequence_{0}, sent_{0}, total_{0};
};
inline Progress& current() { static Progress progress; return progress; }
} }
