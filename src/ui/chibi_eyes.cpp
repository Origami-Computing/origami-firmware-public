#include "ui/chibi_eyes.h"
#include "hal/hal_display.h"
#include "assets/kami_chibi_eyes.h"
#include "ui/alpha_bitmap.h"

namespace ui { namespace chibi_eyes {

const uint8_t* bitmap(Expression expression, int8_t rotationDegrees) {
  using namespace assets::kami_chibi;
  const uint8_t* pixels = kWake;
  switch (expression) {
    case Expression::Wake: pixels = kWake; break;
    case Expression::ShutTight: {
      int angle = rotationDegrees < -14 ? -14 : rotationDegrees > 14 ? 14 : rotationDegrees;
      pixels = kShutTightRotations[(angle + 14) / 2];
      break;
    }
    case Expression::Sleep: pixels = kSleep; break;
    case Expression::Bored: pixels = kBored; break;
    case Expression::Happy: pixels = kHappy; break;
    case Expression::Curious: pixels = kCurious; break;
  }
  return pixels;
}

const char* name(Expression expression) {
  switch (expression) {
    case Expression::Wake: return "wake";
    case Expression::ShutTight: return "shut-tight";
    case Expression::Sleep: return "sleep";
    case Expression::Bored: return "bored";
    case Expression::Happy: return "happy";
    case Expression::Curious: return "curious";
  }
  return "wake";
}

void draw(TFT_eSPI& display, Expression expression, int16_t x, int16_t y,
          uint16_t foreground, uint16_t background) {
  drawAlphaBitmap(display, x, y, kWidth, kHeight, bitmap(expression), kWidth, kHeight,
                  foreground, background);
}

} } // namespace ui::chibi_eyes
