#include "scene/asset_cache.h"

#include <Arduino.h>
#include <string.h>

#include <esp_heap_caps.h>

#include "hal/hal_display.h"
#include "hal/hal_sd.h"

namespace scene {

namespace {

static constexpr int kMaxAssets = 12;

struct Entry {
    char        id[32];
    uint16_t*   pixels;
    int         w;
    int         h;
    AssetPriority priority;
    bool        used;
};

static Entry s_entries[kMaxAssets];

static Entry* findEntry(const char* id) {
    for (int i = 0; i < kMaxAssets; i++) {
        if (s_entries[i].used && strcmp(s_entries[i].id, id) == 0) return &s_entries[i];
    }
    return nullptr;
}

static Entry* allocEntry(const char* id) {
    Entry* e = findEntry(id);
    if (e) return e;
    for (int i = 0; i < kMaxAssets; i++) {
        if (!s_entries[i].used) {
            strncpy(s_entries[i].id, id, sizeof(s_entries[i].id) - 1);
            s_entries[i].id[sizeof(s_entries[i].id) - 1] = '\0';
            s_entries[i].used = true;
            return &s_entries[i];
        }
    }
    for (int i = 0; i < kMaxAssets; i++) {
        if (s_entries[i].priority == AssetPriority::Low) {
            if (s_entries[i].pixels) heap_caps_free(s_entries[i].pixels);
            s_entries[i] = Entry{};
            strncpy(s_entries[i].id, id, sizeof(s_entries[i].id) - 1);
            s_entries[i].id[sizeof(s_entries[i].id) - 1] = '\0';
            s_entries[i].used = true;
            return &s_entries[i];
        }
    }
    return nullptr;
}

}  // namespace

bool assetPreload(const char* contentId, const char* bmpPath,
                  int32_t frameW, int32_t frameH, int32_t startRow,
                  AssetPriority priority) {
    if (!contentId || !bmpPath) return false;
    if (!hal::sd::mounted()) return false;
    Entry* e = allocEntry(contentId);
    if (!e) return false;

    const size_t pixels = (size_t)frameW * (size_t)frameH;
    if (!e->pixels || e->w != frameW || e->h != frameH) {
        if (e->pixels) heap_caps_free(e->pixels);
        e->pixels = (uint16_t*)heap_caps_malloc(pixels * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
        if (!e->pixels) {
            e->pixels = (uint16_t*)heap_caps_malloc(pixels * sizeof(uint16_t), MALLOC_CAP_8BIT);
        }
        if (!e->pixels) return false;
        e->w = frameW;
        e->h = frameH;
    }

    hal::display::releaseSpiForSd();
    if (!hal::sd::exists(bmpPath)) return false;
    if (!hal::sd::loadBmpRegion(bmpPath, e->pixels, frameW, frameH, startRow)) return false;
    e->priority = priority;
    return true;
}

void assetRelease(const char* contentId) {
    Entry* e = findEntry(contentId);
    if (!e) return;
    if (e->pixels) heap_caps_free(e->pixels);
    *e = Entry{};
}

const uint16_t* assetPixels(const char* contentId, int* outW, int* outH) {
    Entry* e = findEntry(contentId);
    if (!e || !e->pixels) return nullptr;
    if (outW) *outW = e->w;
    if (outH) *outH = e->h;
    return e->pixels;
}

const uint16_t* assetPixelsForFrame(const char* contentBaseId, uint8_t frame,
                                    int* outW, int* outH) {
    if (!contentBaseId) return nullptr;
    char id[32];
    snprintf(id, sizeof(id), "%s:%u", contentBaseId, (unsigned)frame);
    return assetPixels(id, outW, outH);
}

void assetCacheClear() {
    for (int i = 0; i < kMaxAssets; i++) {
        if (s_entries[i].pixels) heap_caps_free(s_entries[i].pixels);
        s_entries[i] = Entry{};
    }
}

}  // namespace scene
