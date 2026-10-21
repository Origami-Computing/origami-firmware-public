#include "core/memory.h"

#include <Arduino.h>
#include <TB_TFT_eSPI.h>
#include <esp_heap_caps.h>

namespace core {
namespace memory {

Snapshot snapshot() {
    Snapshot s{};
    s.psramFree    = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s.psramLargest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    s.dramFree     = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s.dramLargest  = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    return s;
}

void logSnapshot(const char* phase) {
    const Snapshot s = snapshot();
    Serial.printf("[mem] phase=%s psramFree=%u psramLargest=%u dramFree=%u dramLargest=%u\n",
                  phase ? phase : "?",
                  (unsigned)s.psramFree, (unsigned)s.psramLargest,
                  (unsigned)s.dramFree,  (unsigned)s.dramLargest);
}

TFT_eSprite* allocSprite(const char* label, TFT_eSPI& parent, int w, int h, uint8_t colorDepth) {
    const char* lbl = label ? label : "sprite";
    TFT_eSprite* spr = new TFT_eSprite(&parent);
    if (!spr) {
        Serial.printf("[mem] sprite alloc FAILED (new): label=%s w=%d h=%d\n", lbl, w, h);
        logSnapshot("sprite.fail");
        return nullptr;
    }
    spr->setColorDepth(colorDepth);
    if (!spr->createSprite(w, h)) {
        Serial.printf("[mem] sprite alloc FAILED (createSprite): label=%s w=%d h=%d depth=%u\n",
                      lbl, w, h, (unsigned)colorDepth);
        logSnapshot("sprite.fail");
        delete spr;
        return nullptr;
    }
    return spr;
}

void freeSprite(TFT_eSprite*& spr) {
    if (!spr) return;
    if (spr->created()) spr->deleteSprite();
    delete spr;
    spr = nullptr;
}

void* allocPsram(const char* label, size_t bytes) {
    const char* lbl = label ? label : "psram";
    void* p = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!p) {
        Serial.printf("[mem] psram alloc FAILED: label=%s bytes=%u\n", lbl, (unsigned)bytes);
        logSnapshot("psram.fail");
    }
    return p;
}

void freePsram(void*& ptr) {
    if (!ptr) return;
    heap_caps_free(ptr);
    ptr = nullptr;
}

}  // namespace memory
}  // namespace core
