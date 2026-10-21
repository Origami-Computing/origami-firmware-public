#pragma once
#include <stddef.h>
#include <stdint.h>
class JPEGDEC;

namespace core { namespace images {
// Synchronous QVGA/VGA JPEG -> centered 280x240 window in native 240x280
// portrait storage, RGB565 big endian. Owns only the decoder workspace; the
// caller owns the destination and must not present an interrupted partial frame.
// UI-task use only: prepare, decode and close must not run concurrently.
class PreviewDecoder {
public:
    enum class Result { Decoded, Interrupted, Invalid };
    using ShouldAbort = bool (*)(void*);
    PreviewDecoder() = default;
    ~PreviewDecoder();
    PreviewDecoder(const PreviewDecoder&) = delete;
    PreviewDecoder& operator=(const PreviewDecoder&) = delete;
    // Call after camera DMA allocation. Keeps internal-first/PSRAM-fallback policy.
    bool prepare();
    void close();
    bool ready() const { return decoder_ != nullptr; }
    Result decode(const uint8_t* jpeg, size_t length, uint16_t* destination,
                  ShouldAbort shouldAbort = nullptr, void* user = nullptr);
    static constexpr size_t kPixels = 280u * 240u;
private:
    JPEGDEC* decoder_ = nullptr;
};
} }
