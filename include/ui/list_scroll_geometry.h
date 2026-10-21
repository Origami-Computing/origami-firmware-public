#pragma once

#include "ui/safe_area_layout.h"

namespace ui {
// Keep the rail in its column; shorten it where the curved bezel narrows.
inline safe_area::Box fitListScrollbar(safe_area::Box box) {
    while (box.h > 0 && !safe_area::fits({box.x, box.y, box.w, 1})) {
        ++box.y;
        --box.h;
    }
    while (box.h > 0 && !safe_area::fits({box.x, box.y + box.h - 1, box.w, 1})) {
        --box.h;
    }
    return box;
}
}  // namespace ui
