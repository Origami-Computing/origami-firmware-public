#pragma once
#include "app.h"
#include <stdint.h>

class Stream;

namespace apps {
namespace photos {
extern App app;
void dumpStatus(Stream& output);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
// Paints a loading screen onto the canvas for screenshots while Photos is open:
// video=false is the card shuffle at time ms; video=true is the video loader
// for the current item's thumbnail. The next gallery redraw replaces it.
bool previewLoaderForDevelopment(bool video, uint32_t ms);
void setWaitAnimationForDevelopment(bool on);
#endif
}  // namespace photos
}  // namespace apps
