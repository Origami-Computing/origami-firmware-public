#pragma once
#include <SD.h>
#include <stdint.h>

namespace core {

class WavWriter {
public:
    bool open(const char* path, uint32_t sampleRate = 48000, uint16_t channels = 1);
    bool write(const int16_t* pcm, int n);  // false if the SD handle/write fails
    bool close();  // flush, finalize, reopen and verify; false on any failure
    bool isOpen() const { return open_; }

private:
    File     file_;
    bool     open_         = false;
    uint32_t totalSamples_ = 0;
    uint16_t channels_     = 1;
    static constexpr size_t kBufBytes = 4096;
    uint8_t  buf_[kBufBytes];
    size_t   fill_ = 0;
    char path_[128] = {};

    bool flushBuf();
    bool flushBufLocked();

public:
    /** Write a 44-byte WAV header for `numSamples` mono 16-bit samples to a raw byte buffer.
     *  Caller must ensure `h` is at least 44 bytes. Matches the layout produced by open(). */
    static void buildRawHeader(uint8_t* h, uint32_t sampleRate, uint32_t numSamples);
};

}  // namespace core
