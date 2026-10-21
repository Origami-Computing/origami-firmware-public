#include "core/preview_decoder.h"
#include <Arduino.h>
#include <JPEGDEC.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <limits.h>
#include <new>

namespace core { namespace images {
namespace {
struct Decode {
    uint16_t* pixels;
    PreviewDecoder::ShouldAbort shouldAbort;
    void* user;
    bool interrupted;
    uint16_t sourceX[280];
    uint16_t sourceY[240];
};
int writeBlock(JPEGDRAW* block) {
    Decode& decode = *static_cast<Decode*>(block->pUser);
    if (decode.shouldAbort && decode.shouldAbort(decode.user)) {
        decode.interrupted = true;
        return 0;
    }
    const int x = block->x, y = block->y;
    const int w = block->iWidthUsed, h = block->iHeight;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > 320 || y + h > 240) return 0;
    const int x0 = std::max(0, x - 20);
    const int x1 = std::min(280, x + w - 20);
    // Preserve the fused center crop + portrait transpose. Destination columns
    // are contiguous in PSRAM; source MCU rows retain the decoder's full stride.
    for (int dx = x0; dx < x1; ++dx) {
        uint16_t* dst = decode.pixels + (279 - dx) * 240;
        const int sx = decode.sourceX[dx] - x;
        for (int dy = y; dy < y + h; ++dy)
            dst[dy] = block->pPixels[(decode.sourceY[dy] - y) * block->iWidth + sx];
    }
    return 1;
}
}
PreviewDecoder::~PreviewDecoder() { close(); }
void PreviewDecoder::close() {
    if (!decoder_) return;
    decoder_->~JPEGDEC();
    heap_caps_free(decoder_);
    decoder_ = nullptr;
}
bool PreviewDecoder::prepare() {
    close();
    void* workspace = heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const bool internal = workspace != nullptr;
    if (!workspace) workspace = heap_caps_malloc(sizeof(JPEGDEC), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!workspace) return false;
    decoder_ = new (workspace) JPEGDEC;
    Serial.printf("[camera-vga] decoder workspace=%s\n", internal ? "internal" : "PSRAM");
    return true;
}
PreviewDecoder::Result PreviewDecoder::decode(const uint8_t* data, size_t length,
        uint16_t* destination, ShouldAbort shouldAbort, void* user) {
    if (!decoder_ || !destination || !data || length > INT32_MAX) return Result::Invalid;
    Decode context{destination, shouldAbort, user, false, {}, {}};
    for (int x = 0; x < 280; ++x) context.sourceX[x] = x + 20;
    for (int y = 0; y < 240; ++y) context.sourceY[y] = y;
    JPEGDEC& jpeg = *decoder_;
    if (!jpeg.openRAM(const_cast<uint8_t*>(data), (int)length, writeBlock)) return Result::Invalid;
    const bool vga = jpeg.getWidth() == 640 && jpeg.getHeight() == 480;
    if (!vga && !(jpeg.getWidth() == 320 && jpeg.getHeight() == 240)) {
        jpeg.close();
        return Result::Invalid;
    }
    jpeg.setUserPointer(&context);
    jpeg.setPixelType(RGB565_BIG_ENDIAN);
    const bool decoded = jpeg.decode(0, 0, vga ? JPEG_SCALE_HALF : 0) != 0;
    jpeg.close();
    if (context.interrupted) return Result::Interrupted;
    return decoded ? Result::Decoded : Result::Invalid;
}
} }
