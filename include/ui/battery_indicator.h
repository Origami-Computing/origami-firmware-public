#pragma once
#include <stdint.h>

namespace ui { namespace battery {
constexpr uint16_t kGreen = 0x4DAA;
constexpr uint16_t kYellow = 0xFE69;
constexpr uint16_t kRed = 0xE986;
constexpr int kFillX = 5, kFillY = 5, kFillW = 29, kFillH = 16;
constexpr uint32_t kFlashHalfMs = 600;
inline bool critical(uint8_t percent) { return percent <= 10; }
inline bool blackPhase(uint8_t percent, bool external, uint32_t now) {
  return critical(percent) && !external && (now / kFlashHalfMs) % 2;
}
inline uint16_t color(uint8_t percent) {
  return percent > 30 ? kGreen : percent > 20 ? kYellow : kRed;
}
inline int fillWidth(uint8_t percent) {
  return percent > 100 ? 0 : (kFillW * percent + 50) / 100;
}
// Smooth 2.4-second breathe while plugged in, including charge-complete.
// 25%-100% opacity: visible fade, but the power indicator never disappears.
inline uint8_t boltAlpha(bool external,uint32_t now) {
  if(!external) return 255;
  static const uint8_t levels[21]={64,65,69,74,82,92,103,116,130,145,160,174,189,203,216,227,237,245,250,254,255};
  const unsigned step=(now/60)%40;
  return levels[step<=20 ? step : 40-step];
}
inline uint16_t blend(uint16_t ink,uint16_t bg,uint8_t a) {
  return (((((ink>>11)&31)*a+((bg>>11)&31)*(255-a)+127)/255)<<11) |
         (((((ink>>5)&63)*a+((bg>>5)&63)*(255-a)+127)/255)<<5) |
         (((ink&31)*a+(bg&31)*(255-a)+127)/255);
}
#ifdef ORIGAMI_DEV_SERIAL_INPUT
// Display-only fixture: expires in two minutes; never alters battery telemetry.
void setPreview(int percent,int power,int phase=-1,bool audio=false);
void printPreviewStatus();
#endif
} }
