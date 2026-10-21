#pragma once
#include <stdint.h>
namespace ui {
struct NativeFramePresentation { uint32_t syncWaitUs=0; bool syncMissed=false; };
// UI-task only. Borrow 240x280 big-endian RGB565 pixels synchronously. Owns
// display lock, VSync and temporary panel state; restores rotation/byte swap.
NativeFramePresentation presentNativeFrame(uint16_t* pixels);
}
