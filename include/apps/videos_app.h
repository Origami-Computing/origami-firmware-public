#pragma once
#include "app.h"
class Stream;
class TFT_eSprite;
namespace apps { namespace videos {
extern App app;
// UI-task navigation into the existing player; copies path before mounting.
bool openFile(const char* path);
void dumpStatus(Stream& output);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
// UI-task diagnostic capture of the actual paused native composite.
bool copyPreviewForDevelopment(TFT_eSprite& target);
#endif
} }
