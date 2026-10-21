#define MINIMP3_IMPLEMENTATION
#include "core/mp3_codec.h"

#include <SD.h>
#include <cstring>
#include <esp_heap_caps.h>

namespace core {
namespace audio {

void Mp3Codec::init(uint8_t* buf, int bufSize) {
    buf_     = buf;
    bufSize_ = bufSize;
    reset();
}

void Mp3Codec::reset() {
    mp3dec_init(&decoder_);
    fill_ = 0;
    pos_  = 0;
}

int Mp3Codec::fillInput(fs::File& file, bool force) {
    if (!buf_) return 0;
    // Compact unconsumed bytes to front.
    if (pos_ > 0) {
        int remaining = fill_ - pos_;
        if (remaining > 0) memmove(buf_, buf_ + pos_, remaining);
        fill_ = remaining;
        pos_  = 0;
    }
    // Only refill when < 50% full — keeps SD SPI transactions large and
    // infrequent, preventing TFT stalls from tiny back-to-back SD reads.
    int space = bufSize_ - fill_;
    if (!force && space < bufSize_ / 2) return 0;
    int totalRead = 0;
    if (file.available()) {
        int rd = file.read(buf_ + fill_, space);
        if (rd > 0) {
            fill_ += rd;
            totalRead += rd;
        }
    }
    return totalRead;
}

int Mp3Codec::decodeFrame(int16_t* outPcm, int /*outMax*/, mp3dec_frame_info_t* info) {
    if (!buf_) return 0;
    memset(info, 0, sizeof(*info));
    int avail = fill_ - pos_;
    if (avail < 16) return -1;
    int samples = mp3dec_decode_frame(&decoder_, buf_ + pos_, avail, outPcm, info);
    pos_ += info->frame_bytes;
    return samples;
}

void Mp3Codec::discardBytes(int count) {
    if (count <= 0) return;
    int avail = fill_ - pos_;
    if (count > avail) count = avail;
    pos_ += count;
}

void Mp3Codec::syncPastGarbage(fs::File& file, int maxFrames) {
    int16_t dummy[MINIMP3_MAX_SAMPLES_PER_FRAME];
    for (int i = 0; i < maxFrames; i++) {
        fillInput(file);
        mp3dec_frame_info_t info;
        decodeFrame(dummy, MINIMP3_MAX_SAMPLES_PER_FRAME, &info);
        if (info.hz > 0) break;
    }
}

void* allocPsramWithFallback(size_t bytes) {
    const uint32_t ps8  = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    const uint32_t ps   = MALLOC_CAP_SPIRAM;
    const uint32_t int8 = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    void* p = heap_caps_malloc(bytes, ps8);
    if (!p) p = heap_caps_malloc(bytes, ps);
    if (!p) p = heap_caps_malloc(bytes, int8);
    return p;
}

}  // namespace audio
}  // namespace core
