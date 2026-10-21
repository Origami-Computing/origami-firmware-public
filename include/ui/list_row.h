#pragma once
#include <stddef.h>
#include <stdint.h>

namespace ui {
/** Data returned per row by the ItemFn callback. */
struct ListItem {
    const char* label   = nullptr; ///< Row text (must outlive the draw() call).
    uint16_t    fg      = 0xFFFF;  ///< Foreground colour when the row is not selected.
    /** true = radio-select style: shows '●' when checked, nothing otherwise.
     *  false (default) = navigation style: always shows '>'. */
    bool choice  = false;
    /** For choice rows only: marks this item as the active selection ('●'). */
    bool checked = false;
    /** Optional fixed indicator colour. Zero keeps the row foreground behaviour. */
    uint16_t indicatorFg = 0;
    bool opensScreen = true; ///< Only navigation rows display a chevron.
    bool editing = false;
    bool blinkOn = true;
    const uint8_t* icon = nullptr;
    uint8_t iconSize = 20; ///< Native square mask size; no runtime scaling.
    uint8_t iconSlotSize = 20; ///< Stable slot while animated frames change size.
    uint16_t iconColor = 0; ///< Zero inherits row foreground.
};

// Shared content, style and anchors. Pass fitted position/width and the
// adapter's alignment height. Font 1 / size 2 metrics follow the selected build font.
// Captions use the existing byte-based font-1 convention and ".." truncation.
struct ListRowPresentation {
    enum class Indicator { None, Chevron, Dot };
    uint16_t foreground, background, indicatorColor;
    Indicator indicator;
    char label[80];
    int textX, textY, textHeight, labelWidth, indicatorX, dotX, dotY;
    const uint8_t* icon;
    int iconX, iconY, iconSize;
    uint16_t iconColor;
};
ListRowPresentation presentListRow(const ListItem& item, bool selected,
                                   int x, int y, int rowWidth, int rowHeight,
                                   uint16_t selectedBackground,
                                   uint16_t selectedForeground, uint16_t background);
} // namespace ui
