#pragma once
#include <stdint.h>
#include <stddef.h>
namespace services { namespace video {
// UI-task, synchronous single-image read under the gallery's SD reservation.
// Owns up to 128 KiB PSRAM; no mic, playback worker, radio or filesystem writes.
// Data is borrowed until clear/load/destruction. Invalid/partial AVIs fail closed.
class Thumbnail {
public:
    ~Thumbnail(){clear();}
    Thumbnail()=default;
    Thumbnail(const Thumbnail&)=delete;
    Thumbnail& operator=(const Thumbnail&)=delete;
    bool load(const char* path);
    void clear();
    const uint8_t* data() const {return data_;}
    size_t size() const {return bytes_;}
    uint32_t durationMs() const {return durationMs_;}
private:
    uint8_t* data_=nullptr;
    uint32_t bytes_=0,durationMs_=0;
};
} }
