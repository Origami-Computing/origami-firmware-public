#pragma once
/**
 * @file core/mp3_codec.h
 * @brief minimp3 codec wrapper: input buffer management + frame decoding.
 *
 * Manages a borrowed bitstream staging buffer and owns minimp3 decoder state.
 * The playback service retains the file handle, PCM ring and control flags.
 *
 * Usage:
 *   Mp3Codec codec;
 *   codec.init(inBuf, inBufSize);        // attach caller-allocated buffer
 *   codec.reset();                        // on track start / seek
 *   codec.fillInput(file);               // call before decodeFrame
 *   int samples = codec.decodeFrame(outBuf, MINIMP3_MAX_SAMPLES_PER_FRAME, &info);
 *   codec.syncPastGarbage(file, 5);      // discard non-frame bytes after seek
 */

#include <stddef.h>
#include <stdint.h>
#include "minimp3.h"

// Forward-declare SD File without pulling in the entire SD header.
// The Arduino ESP32 SDK defines File in namespace fs; the global 'File'
// alias is injected only after <FS.h>/<SD.h> are included, so we use
// the canonical namespace-qualified form here.
namespace fs { class File; }

namespace core {
namespace audio {

class Mp3Codec {
public:
    // Attach a caller-allocated input buffer (e.g. PSRAM, 16 KB).
    // Must be called before any other method.  Does not take ownership.
    void init(uint8_t* buf, int bufSize);

    // Clear decoder state and input buffer cursors.  Call at track start/seek.
    void reset();

    // Compact unconsumed bytes to front of inBuf, then refill from file if
    // buffer is < 50% full.  Batching reads keeps SPI transactions infrequent.
    int fillInput(fs::File& file, bool force = false);

    // Decode one MPEG audio frame from inBuf into outPcm.
    // Returns number of PCM samples (0 = nothing decoded, -1 = need more data).
    // info is populated by minimp3; caller reads info.channels, info.hz, etc.
    int decodeFrame(int16_t* outPcm, int outMax, mp3dec_frame_info_t* info);

    // Skip up to `maxFrames` frames, seeking past garbage after a byte-seek,
    // until a valid sync word is found (info.hz > 0).
    void syncPastGarbage(fs::File& file, int maxFrames);

    bool hasInput() const { return buf_ && bufSize_ > 0; }
    /** Unconsumed compressed bytes still in the staging buffer. */
    int pendingBytes() const { return fill_ - pos_; }
    int capacity() const { return bufSize_; }
    void discardBytes(int count);

private:
    mp3dec_t decoder_{};
    uint8_t* buf_     = nullptr;
    int      bufSize_ = 0;
    int      fill_    = 0;
    int      pos_     = 0;
};

// Allocate `bytes` from PSRAM (8-bit), PSRAM (generic), then internal RAM,
// in that priority order.  Returns nullptr if all three fail.
void* allocPsramWithFallback(size_t bytes);

}  // namespace audio
}  // namespace core
