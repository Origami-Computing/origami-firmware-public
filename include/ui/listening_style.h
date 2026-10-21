#pragma once
#include <stdint.h>
#include "ui/listening_styles_generated.h"

class TFT_eSprite;

namespace ui { namespace listening_style {
int selected();
// Persist only when the user explicitly selects a preset in Settings.
bool select(int index, bool persist = true);
// Build reusable geometry before a voice gesture, outside the capture path.
bool prepare(void (*serviceMic)() = nullptr);
void releaseCache();
// Snapshot the base canvas. Never repeatedly tint previously painted frames.
bool begin(void (*serviceMic)());
// With overlay=true, the caller must freshly draw the underlay each frame;
// the border composites above it instead of restoring the begin() snapshot.
float paint(float envelope, void (*serviceMic)(), float opacity = 1.f, bool force = false, bool overlay = false);
// Composite the browsed style over a freshly drawn full-screen canvas.
// Does not change the selected style, saved settings or live capture caches.
void drawPreview(TFT_eSprite& canvas, int index);
// Restore the preview's clean status-band underlay before transparent chrome.
// UI thread only; exitPreview releases the optional PSRAM snapshot on view exit.
void restorePreviewChrome(TFT_eSprite& canvas, int index);
void exitPreview();
void end();
// Drop the captured home scene when the live character transition starts.
void clearBase();
bool ready();
uint32_t lastRenderUs();
}}
