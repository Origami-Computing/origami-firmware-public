#pragma once

#include <stddef.h>
#include <stdint.h>

class TFT_eSPI;
class TFT_eSprite;

namespace core {
namespace memory {

struct Snapshot {
    size_t psramFree;
    size_t psramLargest;
    size_t dramFree;
    size_t dramLargest;
};

/** Read PSRAM/internal free bytes and largest blocks without logging. */
Snapshot snapshot();

/** Log one snapshot using the stable [mem] phase=... psramFree=...
 * psramLargest=... dramFree=... dramLargest=... format. Null phase becomes "?". */
void logSnapshot(const char* phase);

/** Create a sprite at the requested depth using TFT_eSprite's allocation policy.
 * Logs allocation failures with label (or "sprite") and a heap snapshot.
 * On failure, destroys any partially created object and returns nullptr. */
TFT_eSprite* allocSprite(const char* label, TFT_eSPI& parent, int w, int h, uint8_t colorDepth = 16);

/** Delete the sprite/backing surface and null the pointer. Null is a no-op. */
void freeSprite(TFT_eSprite*& spr);

/** Allocate PSRAM; logs failures with label (or "psram") and a heap snapshot.
 * Returns nullptr on failure. Successful allocations are not logged here. */
void* allocPsram(const char* label, size_t bytes);

/** Free and null a PSRAM pointer. Null is a no-op. No logging. */
void freePsram(void*& ptr);

}  // namespace memory
}  // namespace core
