#pragma once
#include <stddef.h>
#include <stdint.h>
class JPEGDEC;

namespace core { namespace images {
// Synchronous QVGA/VGA JPEG (fitted playback also accepts HD) -> centered 280x240 window in native 240x280
// portrait storage, RGB565 big endian. Optional fit=true preserves the whole
// image with aspect-preserving letterboxing for video playback. Owns decoder workspace and video scratch; the
// caller owns the destination and must not present an interrupted partial frame.
// Single-owner use: prepare, decode and close must not run concurrently. A
// service may hand off exclusive ownership to a worker and join before closing.
class PreviewDecoder {
public:
    enum class Result { Decoded, Interrupted, Invalid };
    using ShouldAbort = bool (*)(void*);
    PreviewDecoder() = default;
    ~PreviewDecoder();
    PreviewDecoder(const PreviewDecoder&) = delete;
    PreviewDecoder& operator=(const PreviewDecoder&) = delete;
    // Call after camera DMA allocation. Keeps internal-first/PSRAM-fallback policy.
    // JPEGDEC reduces VGA by half and fitted HD by one quarter before mapping.
    bool prepare();
    // Default native decoding uses retained, aligned 20 KiB MCU-row scratch
    // for fitted video or a centered crop; HD fitting grows it to 40 KiB
    // on first decode, with allocation failure reported before drawing. VGA crop samples full-resolution
    // pixels; its geometry matches the legacy half-scale crop, but codec
    // rounding/filtering can differ. Closes any
    // prior mode; failure leaves the decoder unprepared. Video cancellation is
    // checked between native MCU-row decodes. Interrupted/failed output must
    // not be presented, as with the camera decoder. Accepts 8-bit baseline JPEG
    // with unit chroma sampling (4:4:4, horizontal 4:2:2, 4:2:0) or grayscale;
    // unsupported layouts are rejected before native decoding.
    // cameraCrop configures whole-frame scale/crop/270-degree rotation directly
    // into a 16-byte-aligned 280x240 destination. No row scratch is allocated;
    // cancellation is checked before/after the indivisible codec call. fit=true
    // is invalid in this mode. Native codec crop/filter parity needs device QA.
    bool prepareNative(bool cameraCrop = false);
    void close();
    bool ready() const { return decoder_ != nullptr || videoDecoder_ != nullptr; }
    Result decode(const uint8_t* jpeg, size_t length, uint16_t* destination,
                  ShouldAbort shouldAbort = nullptr, void* user = nullptr, bool fit = false);
    static constexpr size_t kPixels = 280u * 240u;
private:
    JPEGDEC* decoder_ = nullptr;
    void* videoDecoder_ = nullptr;
    bool cameraCrop_ = false;
    bool reducedVideo_ = false; // Opt-in decoder comparison; retains video preflight.
    uint16_t* videoPixels_ = nullptr;
    size_t videoPixelBytes_ = 0;
    Result decodeVideo(const uint8_t*, size_t, uint16_t*, ShouldAbort, void*, bool fit);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    uint32_t profileCount_=0,profileTotalUs_=0,profileWriteUs_=0,profileBlocks_=0,profileBytes_=0;
#endif
};
} }
