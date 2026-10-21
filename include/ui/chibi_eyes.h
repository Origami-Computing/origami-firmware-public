#pragma once
#include <stdint.h>

class TFT_eSPI;

namespace ui { namespace chibi_eyes {

enum class Expression : uint8_t { Wake, ShutTight, Sleep, Bored, Happy, Curious };
constexpr int16_t kWidth = 120;
constexpr int16_t kHeight = 102;

// 4-bit coverage, high nibble first: 0 = background, 15 = foreground.
// ShutTight supports pre-rendered rotation from -14 to +14 degrees in 2-degree steps.
const uint8_t* bitmap(Expression expression, int8_t rotationDegrees = 0);
const char* name(Expression expression);

// Top-left coordinates in the caller's active viewport. Opaque draw erases the
// previous expression within this rectangle. Caller owns dirty-region reporting.
void draw(TFT_eSPI& display, Expression expression, int16_t x, int16_t y,
          uint16_t foreground = 0x0000, uint16_t background = 0xffff);

} } // namespace ui::chibi_eyes
