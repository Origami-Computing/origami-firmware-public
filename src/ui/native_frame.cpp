#include "ui/native_frame.h"
#include "hal/hal_display.h"
#include <esp_timer.h>
namespace ui {
namespace { NativeFrameOverlay overlay=nullptr; }
void setNativeFrameOverlay(NativeFrameOverlay value){overlay=value;}
NativeFramePresentation presentNativeFrame(uint16_t* pixels) {
    NativeFramePresentation result;
    if(overlay)overlay(pixels);
    // UI-owned scratch/brightness preparation uses no panel or bus state.
    // Let SD workers progress during this full-frame CPU transform. Acquire SPI
    // before TE so a later SD transfer cannot consume the synchronization edge.
    uint16_t* output=hal::display::prepareNativeOutput(pixels);
    auto& display=hal::display::directTft();
    hal::display::beginDirectDraw();
    const bool swap=display.getSwapBytes();
    const uint8_t rotation=display.getRotation();
    display.setRotation(0);
    const uint64_t sync=esp_timer_get_time();
    result.syncMissed=!hal::display::waitForVSync();
    result.syncWaitUs=esp_timer_get_time()-sync;
    const uint64_t transferStarted=esp_timer_get_time();
    display.writecommand(0x21);display.setSwapBytes(false);
    if (output) display.pushImageOutput(0,0,240,280,output);
    else display.pushImage(0,0,240,280,pixels);
    result.transferUs=esp_timer_get_time()-transferStarted;
    display.setSwapBytes(swap);display.setRotation(rotation);
    hal::display::endDirectDraw();
    return result;
}
}
