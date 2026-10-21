#pragma once
#include "ui/safe_area_layout.h"
#include "ui/default_font_metrics.h"
#include "ui/layout_metrics.h"

namespace ui { namespace layout {
// Measure the selected default font; fixed Font-1 boxes clip Instrument digits.
struct ClockLayout {
    safe_area::Box parts[3];
    bool placed;
};
inline ClockLayout clockLayout(int y, bool physicalSurface) {
    int digit=0;
    for(char c='0';c<='9';++c)digit=safe_area::max(digit,default_font::advance(4,c));
    const int pair=2*digit, colon=default_font::advance(4,':'), height=default_font::height(4);
    // Leave room for the alarm editor's padded digit-selection backgrounds.
    const int gap = space(1);
    ClockLayout result{{{16, y, pair, height}, {16+pair+gap, y, colon, height},
                        {16+pair+colon+2*gap, y, pair, height}}, true};
    if (physicalSurface) result.placed = safe_area::fitGroup(result.parts, 3);
    return result;
}
}} // namespace ui::layout
