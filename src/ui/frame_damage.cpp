#include "ui/frame_damage.h"
#include <esp_heap_caps.h>
#include <string.h>

namespace ui {
void FrameDamage::release() {
    if (before_) heap_caps_free(before_);
    before_ = nullptr;
    bytes_ = rowBytes_ = 0;
    rows_ = 0;
    captured_ = false;
}

void FrameDamage::begin(const void* pixels, size_t rowBytes, uint16_t rows) {
    captured_ = false;
    rows_ = rows;
    rowBytes_ = rowBytes;
    if (!rows || !rowBytes || rowBytes > SIZE_MAX / rows) return;
    const size_t wanted = rowBytes * rows;
    if (bytes_ != wanted) {
        if (before_) heap_caps_free(before_);
        before_ = heap_caps_malloc(wanted, MALLOC_CAP_SPIRAM);
        bytes_ = wanted; // Failed allocation is retried only after release/resize.
    }
    if (before_ && pixels) {
        memcpy(before_, pixels, wanted);
        captured_ = true;
    }
}

FrameDamage::Rows FrameDamage::finish(const void* pixels) {
    const bool captured = captured_;
    captured_ = false;
    if (!captured || !pixels) return {0, rows_};
    const auto* before = static_cast<const uint8_t*>(before_);
    const auto* after = static_cast<const uint8_t*>(pixels);
    uint16_t first = rows_, last = 0;
    for (uint16_t y = 0; y < rows_; ++y) {
        if (memcmp(before + size_t(y)*rowBytes_, after + size_t(y)*rowBytes_, rowBytes_)) {
            if (first == rows_) first = y;
            last = y;
        }
    }
    return first == rows_ ? Rows{0, 0} : Rows{first, uint16_t(last-first+1)};
}
}
