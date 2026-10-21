#pragma once

#include "ui/scene.h"
#include "ui/safe_area_layout.h"
#include "config/ui_theme.h"
#include <stdio.h>

namespace ui {
// Shared alarm-style focus: solid selection while browsing, blinking surface
// and blue ink while editing. Use the same renderer for all time editors.
inline void editField(scene::Scene& scene, bool physicalSurface, const char* id, Rect box,
                      const char* value, bool focused, bool editing,
                      bool blinkOn, uint8_t size = 2) {
    // Fit the containing field once so centered text follows its background.
    // Local surfaces retain their coordinates; scene rendering still owns rasterization.
    if (physicalSurface) {
        const auto fitted = safe_area::fitBox({box.x, box.y, box.w, box.h});
        box = {int16_t(fitted.x), int16_t(fitted.y), int16_t(fitted.w), int16_t(fitted.h)};
    }
    const bool selected = focused && (!editing || blinkOn);
    const uint16_t bg = selected ? kUiSelBg : kUiBg;
    const uint16_t fg = focused && editing ? kUiEdit : selected ? kUiSelFg : kUiFg;
    char bgId[48];
    snprintf(bgId, sizeof(bgId), "%s.bg", id);
    scene.addRect(bgId, box, bg, 3, 1);
    scene.addLabel(id, box, value, fg, bg, size, scene::TextAlign::MiddleCenter, 2);
}
}
