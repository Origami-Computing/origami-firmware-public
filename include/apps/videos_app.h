#pragma once
#include "app.h"
#include <stddef.h>
#include <stdint.h>
class Stream;
class TFT_eSprite;
namespace apps { namespace videos {
extern App app;
// UI-task navigation into the existing player; copies path before mounting.
// poster (optional) is the clip's first JPEG frame; it is copied and shown,
// dimmed, with the timeline filling until the first frame is decoded.
bool openFile(const char* path, const uint8_t* poster = nullptr, size_t posterBytes = 0, uint32_t durationMs = 0);
void dumpStatus(Stream& output);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
// UI-task diagnostic capture of the actual paused native composite.
bool copyPreviewForDevelopment(TFT_eSprite& target);
#endif
} }
