#pragma once

namespace ui {
// Geometry only: callers decide visibility and fit the track to their surface
// first. Requires totalItems > visibleRows and totalItems > 0. Both scene and
// immediate renderers retain their own painting and clipping paths.
struct ScrollbarThumb { int y; int height; };
inline ScrollbarThumb scrollbarThumb(int trackY, int trackHeight,
                                     int visibleRows, int totalItems, int first) {
    int height = (trackHeight * visibleRows) / totalItems;
    if (height < 12) height = 12;
    if (height > trackHeight) height = trackHeight;
    const int maxFirst = totalItems - visibleRows;
    const int clamped = first < 0 ? 0 : (first > maxFirst ? maxFirst : first);
    return {trackY + (maxFirst > 0 ? ((trackHeight - height) * clamped) / maxFirst : 0), height};
}
} // namespace ui
