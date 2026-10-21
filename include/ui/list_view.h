#pragma once

/**
 * @file list_view.h
 * @brief Scrollable list widget with virtual-row rendering and a scrollbar.
 *
 * ListView is stateful: it tracks which item is selected and which row is
 * at the top of the visible window (scrollFirst).  Call scroll() to move the
 * selection; clampScroll() adjusts the window so the selection stays visible.
 *
 * Two draw() overloads are provided:
 *
 *   RowPainter overload — legacy, gives the caller full control over how each
 *                         row is drawn (background, text, icons).  Useful for
 *                         rows with complex layouts.
 *
 *   ItemFn overload     — preferred for simple label lists.  The caller
 *                         supplies a lightweight callback returning a ListItem
 *                         {label, fg} for each index.  ListView handles
 *                         selection highlighting automatically using the
 *                         selBg/selFg/rowBg colors passed to draw().
 *
 * The scrollbar is always drawn in the right margin.  It is suppressed
 * visually when all items fit in the visible area (thumb fills the track).
 *
 * Example — ItemFn usage:
 * @code
 *   static ui::ListItem myItemFn(int idx, void* ctx) {
 *       return { kLabels[idx], kUiFg };
 *   }
 *
 *   s_list.totalItems = kCount;
 *   s_list.draw(canvas, 0, topY, canvas.width(), canvas.height() - topY,
 *               myItemFn, nullptr,
 *               kUiSelBg, kUiSelFg, kUiBg, kUiMuted, kUiFg);
 * @endcode
 */

#include <TB_TFT_eSPI.h>
#include <stdint.h>
#include "ui/layout_metrics.h"
#include "ui/list_row.h"

namespace ui {
// UI-task input-dispatch context; reset after each dispatch. Zero is ordinary input.
void setRotaryScrollContext(uint32_t gesture,bool undo);
namespace scene { class Scene; }

// ---------------------------------------------------------------------------
// RowPainter — legacy callback (full custom row rendering)
// ---------------------------------------------------------------------------

/** Called once per visible row by the RowPainter overload of draw().
 *  @param c        TFT surface to draw on (may be a sprite or direct TFT).
 *  @param x, y     Top-left corner of the row's content area.
 *  @param w, h     Width and height of the row's content area.
 *  @param index    Absolute item index in the list (not the slot index).
 *  @param selected True when this row is the current selection.
 *  @param ctx      The opaque pointer passed to draw(). */
typedef void (*RowPainter)(TFT_eSPI& c, int x, int y, int w, int h,
                           int index, bool selected, void* ctx);

// ---------------------------------------------------------------------------
// ItemFn — lightweight label-only callback
// ---------------------------------------------------------------------------

/** Called once per visible row by the ItemFn overload of draw().
 *  The returned pointer only needs to be valid until listRow() returns
 *  (i.e. a static buffer inside the callback is fine). */
using ItemFn = ListItem (*)(int idx, void* ctx);

// ---------------------------------------------------------------------------
// ListView
// ---------------------------------------------------------------------------

struct ListView {
    uint32_t rotaryGesture=0;
    int rotaryBefore=0,rotaryAfter=0;
    int selected    = 0; ///< Index of the currently highlighted item.
    int scrollFirst = 0; ///< Index of the topmost visible item.
    int totalItems  = 0; ///< Total number of items in the list.
    int visibleRows = 0; ///< Computed by draw(); number of rows that fit.

    // Layout parameters — override before calling draw() if you need different spacing.
    int rowH       = layout::kListRowH; ///< Height of the text/content area inside a row.
    int rowPitch   = layout::kListRowPitch; ///< Vertical distance from the top of one row to the next.
    int topPad     = layout::kListTopPad;  ///< Padding above the first row.
    int leftX      = layout::kListLeftInset; ///< Left margin.
    int scrollbarW     = layout::kListScrollbarW;  ///< Scrollbar width in pixels (iPod-style rail).
    int scrollbarPad   = layout::kListScrollbarGap;  ///< Gap between right edge of rows and the scrollbar.
    int scrollbarRight = layout::kListRightInset; ///< Right inset so the scrollbar clears the physical bezel.

    /** Move the selection by delta steps, clamped to [0, totalItems-1],
     *  then update scrollFirst so the selection stays visible. */
    void scroll(int delta);

    /** Adjust scrollFirst so the current selection is within the visible window.
     *  Called automatically by scroll(); call manually after setting selected externally. */
    void clampScroll();

    /** Legacy draw — caller paints each row via the RowPainter callback. */
    void draw(TFT_eSPI& c, int areaX, int areaY, int areaW, int areaH,
              RowPainter painter, void* ctx,
              uint16_t trackColor, uint16_t thumbColor);

    /** ItemFn draw — ListView paints selection highlight; callback supplies label+colour.
     *  @param selBg      Background colour for the selected row.
     *  @param selFg      Text colour for the selected row.
     *  @param rowBg      Background colour for all other rows.
     *  @param trackColor Scrollbar track colour.
     *  @param thumbColor Scrollbar thumb colour. */
    void draw(TFT_eSPI& c, int areaX, int areaY, int areaW, int areaH,
              ItemFn itemFn, void* ctx,
              uint16_t selBg, uint16_t selFg, uint16_t rowBg,
              uint16_t trackColor, uint16_t thumbColor);

    /** Scene build — same layout/scroll behavior as draw(), but emits editable nodes. */
    void buildScene(scene::Scene& scene, int areaX, int areaY, int areaW, int areaH,
                    ItemFn itemFn, void* ctx,
                    uint16_t selBg, uint16_t selFg, uint16_t rowBg,
                    uint16_t trackColor, uint16_t thumbColor);
};

}  // namespace ui
