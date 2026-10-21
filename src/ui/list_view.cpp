/**
 * @file list_view.cpp
 * @brief ListView — scrollable list with optional scrollbar; two draw() overloads.
 *
 * The RowPainter overload is the legacy API used by older call sites.  The newer
 * ItemFn overload takes a lightweight callback that returns a {label, fg} pair and
 * handles selection highlighting internally, keeping all apps free of raw listRow()
 * calls.
 *
 * clampScroll() algorithm
 * -----------------------
 * The invariant is: scrollFirst ≤ selected < scrollFirst + visibleRows.
 *   - If selected scrolled above the window, pull the window up (scrollFirst = selected).
 *   - If selected scrolled below the window, push the window down so selected is the
 *     last visible row.
 * This keeps the selected item always visible without over-scrolling.
 */

#include "ui/list_view.h"
#include "hal/hal_encoder_trace.h"
#include "services/vibration.h"
#include "ui/scrollbar_layout.h"
#include "config/ui_theme.h"
#include "ui/safe_area_draw.h"
#include "ui/draw.h"
#include "ui/scene.h"
#include "ui/list_icon.h"
#include "ui/list_scroll_geometry.h"
#include "sfx/sfx_lib.h"

#include <stdio.h>
#include <string.h>

namespace ui {
static uint32_t inputGesture=0;
static bool inputUndo=false;
void setRotaryScrollContext(uint32_t gesture,bool undo){inputGesture=gesture;inputUndo=undo;}


namespace {

static int fittingRows(int x,int y,int w,int h,int pitch,int available) {
    int rows=0;
    while (rows<available && safe_area::fits({x,y+rows*pitch,w,h})) ++rows;
    return rows>0?rows:1;
}

// One window calculation for custom painting, standard rows and scene nodes.
// Return raw boxes: each renderer retains its existing surface-fitting policy.
struct ListGeometry {
    safe_area::Box firstRow;
    safe_area::Box scrollbar;
};
static ListGeometry prepareLayout(ListView& list, int x, int y, int w, int h) {
    const int topShift = max(0, safe_area::contentTop() - y);
    y += topShift;
    h -= topShift;
    const int rowW = w - list.leftX - list.scrollbarW - list.scrollbarPad - list.scrollbarRight;
    const safe_area::Box first{x + list.leftX, y + list.topPad, rowW, list.rowH};
    list.visibleRows = fittingRows(first.x, first.y, first.w, first.h,
                                  list.rowPitch, (h - list.topPad) / list.rowPitch);
    list.clampScroll();
    return {first, {first.x + rowW + list.scrollbarPad, first.y, list.scrollbarW,
                   list.visibleRows * list.rowPitch - (list.rowPitch - list.rowH)}};
}


}  // namespace

void ListView::clampScroll() {
    if (visibleRows < 1) visibleRows = 1;
    if (totalItems <= 0) {
        selected = 0;
        scrollFirst = 0;
        return;
    }
    if (selected < 0) selected = 0;
    if (selected >= totalItems) selected = totalItems - 1;

    if (totalItems <= visibleRows) {
        // All items fit — no scroll needed, always start from top.
        scrollFirst = 0;
        return;
    }

    const int maxFirst = totalItems - visibleRows;
    if (scrollFirst < 0) scrollFirst = 0;
    if (scrollFirst > maxFirst) scrollFirst = maxFirst;

    if (selected < scrollFirst) scrollFirst = selected;
    if (selected >= scrollFirst + visibleRows) {
        // Push window down so selected lands on the last visible row.
        scrollFirst = selected - (visibleRows - 1);
    }
}

void ListView::scroll(int delta) {
    const int traceBefore=selected;
    struct Observation {ListView& list;int before,delta;~Observation(){hal::input::encoderTraceMenu(before,list.selected,list.totalItems,delta);}} observation{*this,traceBefore,delta};
    if (totalItems <= 0) {
        selected = 0;
        scrollFirst = 0;
        return;
    }
    const int prev = selected;
    if(inputUndo){
        if(!inputGesture || rotaryGesture!=inputGesture || selected!=rotaryAfter)return;
        selected=rotaryBefore;rotaryGesture=0;
    } else {
        rotaryGesture=inputGesture;rotaryBefore=selected;
        selected += delta;
    }
    if (selected < 0) selected = 0;
    if (selected >= totalItems) selected = totalItems - 1;
    clampScroll();
    rotaryAfter=selected;
    if (selected != prev) {
        services::vibration::navigationFeedback(services::vibration::Cue::Scroll);
        sfx::play(sfx::kSfxMenuTick);
    }
}

void ListView::draw(TFT_eSPI& c, int areaX, int areaY, int areaW, int areaH,
                    RowPainter painter, void* ctx,
                    uint16_t trackColor, uint16_t thumbColor) {
    const auto geometry = prepareLayout(*this, areaX, areaY, areaW, areaH);

    for (int slot = 0; slot < visibleRows; slot++) {
        int idx = scrollFirst + slot;
        if (idx >= totalItems) break;
        int ry = geometry.firstRow.y + slot * rowPitch;
        const safe_area::Box raw{geometry.firstRow.x,ry,geometry.firstRow.w,rowH};
        const auto row = safe_area::applies(c) ? safe_area::fitBox(raw) : raw;
        painter(c, row.x, row.y, row.w, row.h, idx, idx == selected, ctx);
    }

    const auto& track = geometry.scrollbar;
    scrollbar(c, track.x, track.y, track.h, track.w,
              visibleRows, totalItems, scrollFirst,
              trackColor, thumbColor);
}

void ListView::draw(TFT_eSPI& c, int areaX, int areaY, int areaW, int areaH,
                    ItemFn itemFn, void* ctx,
                    uint16_t selBg, uint16_t selFg, uint16_t rowBg,
                    uint16_t trackColor, uint16_t thumbColor) {
    const auto geometry = prepareLayout(*this, areaX, areaY, areaW, areaH);

    for (int slot = 0; slot < visibleRows; slot++) {
        int idx = scrollFirst + slot;
        if (idx >= totalItems) break;
        bool      sel  = (idx == selected);
        ListItem  item = itemFn(idx, ctx);
        int       ry   = geometry.firstRow.y + slot * rowPitch;
        listRow(c, geometry.firstRow.x, ry, geometry.firstRow.w, rowH,
                item, sel, selBg, selFg, rowBg);
    }

    const auto& track = geometry.scrollbar;
    scrollbar(c, track.x, track.y, track.h, track.w,
              visibleRows, totalItems, scrollFirst,
              trackColor, thumbColor);
}

void ListView::buildScene(scene::Scene& scene, int areaX, int areaY, int areaW, int areaH,
                          ItemFn itemFn, void* ctx,
                          uint16_t selBg, uint16_t selFg, uint16_t rowBg,
                          uint16_t trackColor, uint16_t thumbColor) {
    const auto geometry = prepareLayout(*this, areaX, areaY, areaW, areaH);

    for (int slot = 0; slot < visibleRows; slot++) {
        const int idx = scrollFirst + slot;
        if (idx >= totalItems) break;
        const bool sel = (idx == selected);
        const ListItem item = itemFn(idx, ctx);
        // Fit the row box; retain nominal rowH for the scene adapter's text alignment.
        const auto row = safe_area::fitBox({geometry.firstRow.x,geometry.firstRow.y+slot*rowPitch,geometry.firstRow.w,rowH});
        const int x = row.x;
        const int y = row.y;
        const int fittedW = row.w;
        const auto presentation = presentListRow(item,sel,x,y,fittedW,rowH,selBg,selFg,rowBg);
        const uint16_t bg = presentation.background;
        const uint16_t fg = presentation.foreground;

        char id[32];
        snprintf(id, sizeof(id), "list.row.%d.bg", idx);
        scene.addRect(id, {(int16_t)x, (int16_t)y, (int16_t)fittedW, (int16_t)row.h}, bg, 5, 1);


        if (presentation.icon) {
            snprintf(id,sizeof(id),"list.row.%d.icon",idx);
            scene.addBitmap(id,{(int16_t)presentation.iconX,(int16_t)presentation.iconY,(int16_t)presentation.iconSize,(int16_t)presentation.iconSize},
                presentation.icon,presentation.iconSize,presentation.iconSize,presentation.iconColor,bg,2,4);
        }
        snprintf(id, sizeof(id), "list.row.%d.label", idx);
        auto* label = scene.addLabel(id, {(int16_t)presentation.textX, (int16_t)presentation.textY,
                            (int16_t)(presentation.labelWidth-layout::kListTextGap), (int16_t)presentation.textHeight},
                       sel ? item.label : presentation.label, fg, bg, 2, scene::TextAlign::TopLeft, 2);
        if (label) label->scrollOnFocus = sel;

        if (presentation.indicator == ListRowPresentation::Indicator::Chevron) {
            snprintf(id, sizeof(id), "list.row.%d.next", idx);
            scene.addLabel(id, {(int16_t)presentation.indicatorX, (int16_t)presentation.textY, layout::kListIndicatorW, (int16_t)presentation.textHeight},
                           ">", fg, bg, 2, scene::TextAlign::TopLeft, 2);
        } else if (presentation.indicator == ListRowPresentation::Indicator::Dot) {
            snprintf(id, sizeof(id), "list.row.%d.dot", idx);
            scene::Node* dot = scene.addRect(id, {(int16_t)(presentation.dotX - 4), (int16_t)(presentation.dotY - 4), 8, 8},
                                             presentation.indicatorColor, 4, 2);
            if (dot) dot->radius = 4;
        }
    }

    if (totalItems <= visibleRows) return;

    const auto track = fitListScrollbar(geometry.scrollbar);
    if (track.h <= 0) return;
    const int sbX = track.x;
    const int sbY = track.y;
    const int sbH = track.h;
    const auto thumb = scrollbarThumb(sbY, sbH, visibleRows, totalItems, scrollFirst);

    scene.addRect("list.scroll.track", {(int16_t)sbX, (int16_t)sbY, (int16_t)track.w, (int16_t)sbH},
                  kUiScrollTrack, 2, 1);
    scene.addRect("list.scroll.thumb", {(int16_t)sbX, (int16_t)thumb.y, (int16_t)track.w, (int16_t)thumb.height},
                  kUiScrollThumb, 2, 2);
}

}  // namespace ui
