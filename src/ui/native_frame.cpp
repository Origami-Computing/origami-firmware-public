#include "ui/native_frame.h"
#include "hal/hal_display.h"
#include <esp_timer.h>
namespace ui {
NativeFramePresentation presentNativeFrame(uint16_t* pixels) {
    NativeFramePresentation result;
    auto& display=hal::display::directTft();
    hal::display::beginDirectDraw();
    const bool swap=display.getSwapBytes();
    const uint8_t rotation=display.getRotation();
    display.setRotation(0);
    const uint64_t sync=esp_timer_get_time();
    result.syncMissed=!hal::display::waitForVSync();
    result.syncWaitUs=esp_timer_get_time()-sync;
    display.writecommand(0x21);display.setSwapBytes(false);
    display.pushImage(0,0,240,280,pixels);
    display.setSwapBytes(swap);display.setRotation(rotation);
    hal::display::endDirectDraw();
    return result;
}
}
