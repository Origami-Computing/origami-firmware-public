#include "apps/photos_cards.h"

#include <Arduino.h>
#include "hal/hal_display.h"
#include <esp_heap_caps.h>
#include <stddef.h>
#include <string.h>
#include <algorithm>
#include "hal/hal_sd.h"

namespace apps {
namespace photos {
namespace cards {

namespace loader = ui::photos_loader;

static constexpr const char* kPath = "/.photos_cards.bin";
static constexpr uint32_t kMagic = 0x31435050;  // "PPC1"

struct Entry {
    uint32_t key;
    uint32_t modified;
    uint16_t px[loader::kCardPixels];
};
struct Store {
    uint32_t magic;
    uint32_t count;
    Entry entries[kMax];
};

static Store* s_store = nullptr;
static uint16_t* s_placeholder = nullptr;
static bool s_loaded = false;
static bool s_dirty = false;

static uint32_t keyOf(const char* path) {
    uint32_t h = 2166136261u;
    for (; *path; ++path) h = (h ^ uint8_t(*path)) * 16777619u;
    return h;
}

static bool ensureMemory() {
    if (!s_store) {
        s_store = static_cast<Store*>(heap_caps_calloc(1, sizeof(Store) + 1, MALLOC_CAP_SPIRAM));
        if (s_store) s_store->magic = kMagic;
    }
    if (!s_placeholder) {
        s_placeholder = static_cast<uint16_t*>(heap_caps_malloc(loader::kCardPixels * 2, MALLOC_CAP_SPIRAM));
        if (s_placeholder) loader::placeholder(s_placeholder);
    }
    return s_store && s_placeholder;
}

void load() {
    if (s_loaded || !ensureMemory()) return;
    s_loaded = true;
    if (!hal::sd::mounted()) return;
    const uint32_t started = millis();
    // readFile NUL-terminates, so it reads at most sizeof(Store) bytes here.
    const size_t got = hal::sd::readFile(kPath, reinterpret_cast<char*>(s_store), sizeof(Store) + 1);
    const size_t header = offsetof(Store, entries);
    if (got < header || s_store->magic != kMagic || s_store->count > uint32_t(kMax) ||
        got != header + s_store->count * sizeof(Entry)) {
        memset(s_store, 0, sizeof(Store));
        s_store->magic = kMagic;
    }
    Serial.printf("[photos-cards] load count=%lu bytes=%u ms=%lu\n", (unsigned long)s_store->count,
                  (unsigned)got, (unsigned long)(millis() - started));
}

loader::Deck deck() {
    loader::Deck d{};
    if (!ensureMemory()) return d;
    for (uint32_t i = 0; i < s_store->count && d.count < kMax; ++i) d.cards[d.count++] = s_store->entries[i].px;
    while (d.count < 3) d.cards[d.count++] = s_placeholder;
    return d;
}

void capture(TFT_eSprite& canvas, int index, const char* path, uint64_t modified, int top, int bottom) {
    if (index < 0 || index >= kMax || !path || !ensureMemory()) return;
    const uint32_t key = keyOf(path), mod = uint32_t(modified);
    uint32_t slot = s_store->count;
    for (uint32_t i = 0; i < s_store->count; ++i)
        if (s_store->entries[i].key == key) {
            if (s_store->entries[i].modified == mod) return;  // already cached
            slot = i;
            break;
        }
    if (slot >= uint32_t(kMax)) return;  // full; keepAndSave makes room on exit

    // Crop the image area to the card's aspect ratio, centred, then box-average.
    const int W = canvas.width();
    const int imgH = bottom - top;
    if (W <= 0 || imgH <= 8) return;
    float cw = float(W), ch = float(imgH), x0 = 0, y0 = float(top);
    const float aspect = float(loader::kCardW) / loader::kCardH;
    if (cw / ch > aspect) { cw = ch * aspect; x0 = (W - cw) / 2; }
    else { ch = cw / aspect; y0 = top + (imgH - ch) / 2; }

    Entry& e = s_store->entries[slot];
    for (int oy = 0; oy < loader::kCardH; ++oy) {
        const int sy0 = int(y0 + ch * oy / loader::kCardH), sy1 = std::max(sy0 + 1, int(y0 + ch * (oy + 1) / loader::kCardH));
        for (int ox = 0; ox < loader::kCardW; ++ox) {
            const int sx0 = int(x0 + cw * ox / loader::kCardW), sx1 = std::max(sx0 + 1, int(x0 + cw * (ox + 1) / loader::kCardW));
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int y = sy0; y < sy1; ++y)
                for (int x = sx0; x < sx1; ++x) {
                    const uint16_t p = canvas.readPixel(x, y);
                    r += p >> 11; g += (p >> 5) & 63; b += p & 31; ++n;
                }
            e.px[oy * loader::kCardW + ox] = uint16_t(((r / n) << 11) | ((g / n) << 5) | (b / n));
        }
    }
    e.key = key;
    e.modified = mod;
    if (slot == s_store->count) ++s_store->count;
    s_dirty = true;
    Serial.printf("[photos-cards] capture index=%d slot=%lu path=%s\n", index, (unsigned long)slot, path);
}

void keepAndSave(const Key* newest, int count) {
    if (!s_store) return;
    if (count > kMax) count = kMax;
    // Rebuild in library order, keeping only cached entries that are still newest.
    uint32_t kept = 0;
    for (int i = 0; i < count; ++i) {
        const uint32_t key = keyOf(newest[i].path), mod = uint32_t(newest[i].modified);
        for (uint32_t j = kept; j < s_store->count; ++j) {
            const Entry& e = s_store->entries[j];
            if (e.key != key || e.modified != mod) continue;
            if (j != kept) {
                // Word-wise swap: no 8.5 KiB temporary in internal RAM.
                uint32_t* a = reinterpret_cast<uint32_t*>(&s_store->entries[kept]);
                uint32_t* b = reinterpret_cast<uint32_t*>(&s_store->entries[j]);
                for (size_t w = 0; w < sizeof(Entry) / 4; ++w) std::swap(a[w], b[w]);
                s_dirty = true;
            }
            ++kept;
            break;
        }
    }
    if (kept != s_store->count) { s_store->count = kept; s_dirty = true; }
    if (!s_dirty || !hal::sd::mounted()) return;
    const uint32_t started = millis();
    const size_t bytes = offsetof(Store, entries) + kept * sizeof(Entry);
    const bool ok = hal::sd::writeFile(kPath, reinterpret_cast<const uint8_t*>(s_store), bytes);
    if (ok) s_dirty = false;
    Serial.printf("[photos-cards] save ok=%d count=%lu bytes=%u ms=%lu\n", ok ? 1 : 0, (unsigned long)kept,
                  (unsigned)bytes, (unsigned long)(millis() - started));
}

}  // namespace cards
}  // namespace photos
}  // namespace apps
