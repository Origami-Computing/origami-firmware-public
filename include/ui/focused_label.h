#pragma once
#include <stdint.h>
class TFT_eSPI;
namespace ui { namespace focused_label {
// One visible menu focus, owned by the UI thread. No scene-builder side effects.
// Navigation brackets actual framebuffer drawing; previews/local sprites cannot
// register a focus. finishFrame repaints only the text band when its offset changes.
void reset();
// Input restarts dwell without losing focus when the app ignores the event.
void restart(uint32_t now);
void beginFrame(TFT_eSPI& surface, const void* screen, uint32_t now);
void contentPainted(TFT_eSPI& surface);
void observe(TFT_eSPI& surface, const char* text, int x, int y, int w, int h,
             uint16_t fg, uint16_t bg, uint8_t font = 1, uint8_t size = 2);
bool finishFrame(TFT_eSPI& surface);
// Music's 1.6 second endpoint pause, 25 pixels/second, back-and-forth motion.
inline int offset(uint32_t elapsed, int overflow) {
    if (overflow <= 0) return 0;
    const uint32_t travel = uint32_t(overflow) * 40;
    const uint32_t phase = elapsed % (3200 + 2 * travel);
    if (phase < 1600) return 0;
    if (phase < 1600 + travel) return (phase - 1600) / 40;
    if (phase < 3200 + travel) return overflow;
    return overflow - (phase - 3200 - travel) / 40;
}
} }
