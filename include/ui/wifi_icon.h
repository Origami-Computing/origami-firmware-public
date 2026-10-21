#pragma once
#include <stdint.h>
class TFT_eSPI;
namespace ui {
constexpr int kWifiIconSize = 22;
// Tier 1=dot only, 2=one arc, 3=two arcs, 4=all arcs; disconnected=muted slash.
void drawWifiIcon(TFT_eSPI& display, int16_t x, int16_t y, int8_t tier, bool connected,
                  uint16_t foreground, uint16_t background, bool transparent = false);
}
