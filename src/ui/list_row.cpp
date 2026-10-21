#include "ui/list_row.h"
#include "ui/default_font_metrics.h"
#include "ui/list_icon.h"
#include "ui/layout_metrics.h"
#include "config/ui_theme.h"
#include <string.h>

namespace ui {
ListRowPresentation presentListRow(const ListItem& item, bool selected,
                                   int x, int y, int rowWidth, int rowHeight,
                                   uint16_t selectedBackground,
                                   uint16_t selectedForeground, uint16_t background) {
    ListRowPresentation row{};
    row.textX = x + layout::kListTextInset;
    row.textHeight = default_font::height(2);
    row.textY = y + (rowHeight - row.textHeight) / 2;
    row.labelWidth = rowWidth - layout::kListTextInset - layout::kListIndicatorInset - layout::kListIndicatorW;
    if (item.icon && rowHeight >= item.iconSize && rowWidth >= 80) {
        row.icon = item.icon;
        row.iconSize = item.iconSize;
        const int slot = item.iconSlotSize > item.iconSize ? item.iconSlotSize : item.iconSize;
        row.iconX = row.textX + (slot-item.iconSize)/2;
        row.iconY = y + (rowHeight-item.iconSize)/2;
        const int advance = slot+4 > kListIconTextOffset ? slot+4 : kListIconTextOffset;
        row.textX += advance; row.labelWidth -= advance;
    }
    row.indicatorX = x + rowWidth - layout::kListIndicatorInset - layout::kListIndicatorW;
    row.dotX = row.indicatorX + layout::kListIndicatorW / 2;
    row.dotY = y + rowHeight / 2;
    row.background = selected && (!item.editing || item.blinkOn) ? selectedBackground : background;
    row.foreground = selected && item.editing ? kUiEdit : selected ? selectedForeground : item.fg;
    row.iconColor = item.iconColor ? item.iconColor : row.foreground;
    row.indicatorColor = item.indicatorFg ? item.indicatorFg : row.foreground;
    row.indicator = !item.choice && item.opensScreen ? ListRowPresentation::Indicator::Chevron :
                    item.checked ? ListRowPresentation::Indicator::Dot : ListRowPresentation::Indicator::None;
    const char* text = item.label ? item.label : "";
#if defined(ORIGAMI_SOFT_BLOCK_DISPLAY) || defined(ORIGAMI_INSTRUMENT_FONT)
    const int available = row.labelWidth - layout::kListTextGap;
    const size_t len = strlen(text);
    if (len < sizeof(row.label) && default_font::width(text,2) <= available) {
        memcpy(row.label,text,len+1);
    } else {
        const int budget=available-default_font::width("..",2);
        size_t n=0; int used=0;
        while (text[n]) {
            uint16_t ch; const size_t bytes=default_font::character(text+n,ch);
            const int advance=default_font::advance(2,ch);
            if (used+advance>budget || n+bytes>sizeof(row.label)-3) break;
            used+=advance; n+=bytes;
        }
        memcpy(row.label,text,n);
        row.label[n]='.'; row.label[n+1]='.';
    }
#else
    const int maxChars = (row.labelWidth - layout::kListTextGap) / 12;
    const size_t len = strlen(text);
    if (!len || (maxChars >= 0 && len <= (size_t)maxChars)) {
        strncpy(row.label, text, sizeof(row.label)-1);
    } else {
        int n = maxChars >= 3 ? maxChars-2 : 0;
        if (n > (int)sizeof(row.label)-3) n = sizeof(row.label)-3;
        memcpy(row.label, text, (size_t)n);
        row.label[n] = '.';
        row.label[n+1] = '.';
    }
#endif
    return row;
}
} // namespace ui
