#pragma once

#include <stdint.h>

namespace scene {

enum class AssetPriority : uint8_t { High, Low };

/** Preload a vertical strip frame from a 24-bit BMP on SD (never read in render). */
bool assetPreload(const char* contentId, const char* bmpPath,
                  int32_t frameW, int32_t frameH, int32_t startRow,
                  AssetPriority priority);

void assetRelease(const char* contentId);

/** RGB565 buffer for one frame; nullptr if missing. */
const uint16_t* assetPixels(const char* contentId, int* outW, int* outH);

/** Lookup preloaded frame: content base id + frame index (e.g. "boot/logo", 3 → "boot/logo:3"). */
const uint16_t* assetPixelsForFrame(const char* contentBaseId, uint8_t frame,
                                  int* outW, int* outH);

void assetCacheClear();

}  // namespace scene
