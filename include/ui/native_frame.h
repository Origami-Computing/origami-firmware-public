#pragma once
#include <stdint.h>
namespace ui {
struct NativeFramePresentation { uint32_t syncWaitUs=0; bool syncMissed=false; uint32_t transferUs=0; };
// UI-task only. Borrow 240x280 big-endian RGB565 pixels synchronously. Owns
// display lock, VSync and temporary panel state; restores rotation/byte swap.
// Optional UI-task composition hook. Owner clears it before teardown; the callback
// may paint into the borrowed native pixels, but must not transfer or navigate.
using NativeFrameOverlay = void (*)(uint16_t* pixels);
void setNativeFrameOverlay(NativeFrameOverlay overlay);
NativeFramePresentation presentNativeFrame(uint16_t* pixels);
}
