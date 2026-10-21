#pragma once
#include "ui/layout_metrics.h"
#include "ui/safe_area_layout.h"
#include "ui/default_font_metrics.h"

namespace ui { namespace layout {
// Fit the containing row first, then derive its label. Independent fitting
// would lose the existing 6 px inset at the physical display's curved edge.
struct ToggleRowLayout { safe_area::Box background, label; };
inline ToggleRowLayout toggleRowLayout(int width, int y, bool physicalSurface) {
    safe_area::Box row{8, y, width - 16, kOnOffRowH - 2};
    if (physicalSurface) row = safe_area::fitBox(row);
    const int textH=default_font::height(2);
    return {row, {row.x + 6, row.y + (kOnOffRowH-textH)/2, row.w - 12, textH}};
}
}} // namespace ui::layout
