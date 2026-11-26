#pragma once
// Card-size thumbnails of the newest photos, shown by the Photos loading
// screen while the library is not readable yet. UI task only.
//
// Thumbnails are captured from the gallery's own full-bleed render (no extra
// JPEG decode), kept in PSRAM for the boot, and saved to one small SD file
// when Photos closes so the next boot can show them right away.
#include <stdint.h>
#include "ui/photos_loader.h"

class TFT_eSprite;

namespace apps {
namespace photos {
namespace cards {

constexpr int kMax = 6;

// Reads the saved thumbnails once per boot. Safe to call on every open.
void load();
// Cached thumbnails newest first, padded with placeholders to at least 3.
ui::photos_loader::Deck deck();
// Called after the gallery draws item `index` full-bleed, before any chrome.
// rows are the image's top/bottom rows on the 280x240 canvas.
void capture(TFT_eSprite& canvas, int index, const char* path, uint64_t modified, int top, int bottom);
// Keeps only the given newest items (in this order) and saves if anything changed.
struct Key { const char* path; uint64_t modified; };
void keepAndSave(const Key* newest, int count);

}  // namespace cards
}  // namespace photos
}  // namespace apps
