#include "ui/clock_layout.h"
#include "ui/draw.h"
#include "ui/alpha_bitmap.h"
#include "ui/list_icon.h"
#include "ui/scrollbar_layout.h"
#include "ui/safe_area_draw.h"
#include "ui/toggle_row_layout.h"
#include "ui/list_scroll_geometry.h"
#include "ui/focused_label.h"

#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "hal/hal_display.h"
#include "nav_handler.h"
#include <stdio.h>
#include <cstring>

namespace ui {

void paintAppOpeningSplash(TFT_eSprite& canvas, const char* appName) {
    if (!appName) appName = "App";

    Painter s(canvas);
    s.clear();

    const int sw         = canvas.width();
    const int contentTop = kSystemStatusBarH;
    const int contentH   = canvas.height() - contentTop;
    const int midY       = contentTop + contentH / 2;

    TFT_eSPI& c = s.raw();
    c.setTextFont(1);
    c.setTextDatum(MC_DATUM);
    c.setTextWrap(false, false);

    c.setTextSize(1);
    c.setTextColor(kUiMuted, kUiBg);
    ui::safe_area::drawString(c, "Opening", sw / 2, midY - 36);

    c.setTextSize(3);
    c.setTextColor(kUiFg, kUiBg);
    ui::safe_area::drawString(c, appName, sw / 2, midY - 6);

    c.setTextSize(1);
    c.setTextColor(kUiAccent, kUiBg);
    ui::safe_area::drawString(c, "...", sw / 2, midY + 28);

    c.setTextDatum(TL_DATUM);
}

void showAppOpeningSplash(const char* appName) {
    if (hal::display::isOptimizedRender()) return;
    paintAppOpeningSplash(hal::display::canvas(), appName);
    hal::display::markAllDirty();
    hal::display::flush();
    NavHandler::instance().invalidateChrome();
    NavHandler::instance().chrome().paint(hal::display::tft());
}

// ---------------------------------------------------------------------------
// Low-level primitives
// ---------------------------------------------------------------------------

void fill(TFT_eSPI& c, Rect r, uint16_t color) {
    c.fillRect(r.x, r.y, r.w, r.h, color);
}

void listRow(TFT_eSPI& c, int x, int y, int w, int h,
             const char* text, bool selected,
             uint16_t selBg, uint16_t selFg, uint16_t bg, uint16_t fg,
             bool choice, bool checked, uint16_t indicatorFg, bool opensScreen) {
    ListItem item;
    item.label=text; item.fg=fg; item.choice=choice; item.checked=checked;
    item.indicatorFg=indicatorFg; item.opensScreen=opensScreen;
    listRow(c,x,y,w,h,item,selected,selBg,selFg,bg);
}

void listRow(TFT_eSPI& c, int x, int y, int w, int h,
             const ListItem& item, bool selected,
             uint16_t selBg, uint16_t selFg, uint16_t bg) {
    if (safe_area::applies(c)) {
        const auto row = safe_area::fitBox({x,y,w,h});
        x=row.x; y=row.y; w=row.w; h=row.h;
    }
    const auto presentation = presentListRow(item,selected,x,y,w,h,selBg,selFg,bg);
    focused_label::contentPainted(c);
    if (selected) focused_label::observe(c,item.label,presentation.textX,presentation.textY,
        presentation.labelWidth-layout::kListTextGap,presentation.textHeight,
        presentation.foreground,presentation.background);
    const uint16_t rowBg = presentation.background;
    const uint16_t rowFg = presentation.foreground;
    c.fillRect(x, y, w, h, bg);
    if (selected) c.fillRoundRect(x, y, w, h, 5, rowBg);
    c.setTextFont(1);
    c.setTextDatum(TL_DATUM);
    c.setTextSize(2);
    c.setTextColor(rowFg, rowBg);

    if (presentation.icon) drawAlphaBitmap(c,presentation.iconX,presentation.iconY,presentation.iconSize,presentation.iconSize,
        presentation.icon,presentation.iconSize,presentation.iconSize,presentation.iconColor,rowBg);
    if (presentation.label[0]) {
        c.setCursor(presentation.textX, presentation.textY);
        c.print(presentation.label);
    }

    // Right-side indicator — inverts automatically because it uses rowFg/rowBg.
    if (presentation.indicator == ListRowPresentation::Indicator::Chevron) {
        // Navigation row: always show '>'.
        c.setCursor(presentation.indicatorX, presentation.textY);
        c.print(">");
    } else if (presentation.indicator == ListRowPresentation::Indicator::Dot) {
        // Radio row, active choice: filled circle dot centred in the indicator zone.
        c.fillCircle(presentation.dotX, presentation.dotY, 4, presentation.indicatorColor);
    }
    // choice && !checked: no indicator — empty right zone.
}

void scrollbar(TFT_eSPI& c, int x, int y, int trackH, int trackW,
               int visibleRows, int totalItems, int scrollFirst,
               uint16_t trackColor, uint16_t thumbColor) {
    if (totalItems <= visibleRows || trackH <= 0) return;
    if (safe_area::applies(c)) {
        const auto box = fitListScrollbar({x,y,trackW,trackH});
        x=box.x; y=box.y; trackW=box.w; trackH=box.h;
    }
    if (trackH <= 0) return;
    // Full-height track — always visible so the user knows there are more items.
    c.fillRoundRect(x, y, trackW, trackH, 2, kUiScrollTrack);
    // Thumb proportional to visible fraction; minimum 12 px so it stays grabbable.
    const auto thumb = scrollbarThumb(y, trackH, visibleRows, totalItems, scrollFirst);
    // Solid thumb flush with track edges — no 1 px inset so it reads as a distinct block.
    c.fillRoundRect(x, thumb.y, trackW, thumb.height, 2, kUiScrollThumb);
}

// ---------------------------------------------------------------------------
// Shared layout metrics and local colours
// ---------------------------------------------------------------------------



// Progress bar fill colour (green)
static constexpr uint16_t kProgressFill  = 0x03A0;
static constexpr uint16_t kProgressTrack = kUiPanel;
// "Connected / OK" blue (iOS-style)
static constexpr uint16_t kOkColor = kUiWifiConnected;

// ---------------------------------------------------------------------------
// Painter implementation
// ---------------------------------------------------------------------------

Painter::Painter(TFT_eSPI& c) : c_(c), curY_(kSystemStatusBarH) {}

void Painter::clear() {
    c_.fillRect(0, kSystemStatusBarH, c_.width(), c_.height() - kSystemStatusBarH, kUiBg);
    curY_ = kSystemStatusBarH;
}

void Painter::panelHeader(const char* text) {
    c_.setTextFont(1);
    c_.setTextDatum(TL_DATUM);
    c_.setTextSize(2);
    c_.setTextColor(kUiFg, kUiBg);
    ui::safe_area::drawString(c_, text, 14, curY_ + 6);
    curY_ += 30;
}

void Painter::listRow(const char* text, bool selected, bool marked) {
    char buf[64];
    if (marked) {
        snprintf(buf, sizeof(buf), "* %s", text);
        text = buf;
    }
    ui::listRow(c_, 8, curY_, c_.width() - 16, layout::kRowH - 2,
                text, selected, kUiSelBg, kUiSelFg, kUiBg, kUiFg);
    curY_ += layout::kRowH;
}

void Painter::onOffRow(const char* text, bool on, bool selected) {
    const auto row = layout::toggleRowLayout(c_.width(), curY_, safe_area::applies(c_));
    char buf[40];
    snprintf(buf, sizeof(buf), "%s %s", text, on ? "ON" : "off");
    uint16_t rbg = selected ? kUiSelBg : kUiBg;
    uint16_t rfg = selected ? kUiSelFg : (on ? kUiFg : kUiMuted);
    c_.fillRect(row.background.x, row.background.y, row.background.w, row.background.h, rbg);
    c_.setTextFont(1);
    c_.setTextDatum(TL_DATUM);
    c_.setTextSize(2);
    c_.setTextColor(rfg, rbg);
    safe_area::drawLabel(c_,buf,row.label,TL_DATUM);
    curY_ += layout::kOnOffRowH;
}

void Painter::emptyState(const char* line1, const char* line2) {
    c_.setTextFont(1);
    c_.setTextDatum(TL_DATUM);
    c_.setTextSize(1);
    c_.setTextColor(kUiMuted, kUiBg);
    ui::safe_area::drawString(c_, line1, 14, curY_ + 4);
    curY_ += layout::kLabelH;
    if (line2 && line2[0]) {
        ui::safe_area::drawString(c_, line2, 14, curY_ + 4);
        curY_ += layout::kLabelH;
    }
}

void Painter::statusLine(const char* text) {
    c_.setTextFont(1);
    c_.setTextDatum(TL_DATUM);
    c_.setTextSize(1);
    c_.setTextColor(kUiFg, kUiBg);
    c_.fillRect(0, curY_, c_.width(), layout::kStatusLineH, kUiBg);
    ui::safe_area::drawString(c_, text, 10, curY_ + 3);
    curY_ += layout::kStatusLineH;
}

void Painter::statusLineOk(const char* text) {
    c_.setTextFont(1);
    c_.setTextDatum(TL_DATUM);
    c_.setTextSize(1);
    c_.setTextColor(kOkColor, kUiBg);
    c_.fillRect(0, curY_, c_.width(), layout::kStatusLineH, kUiBg);
    ui::safe_area::drawString(c_, text, 10, curY_ + 3);
    curY_ += layout::kStatusLineH;
}

void Painter::statusLineMuted(const char* text) {
    c_.setTextFont(1);
    c_.setTextDatum(TL_DATUM);
    c_.setTextSize(1);
    c_.setTextColor(kUiMuted, kUiBg);
    c_.fillRect(0, curY_, c_.width(), layout::kStatusLineH, kUiBg);
    ui::safe_area::drawString(c_, text, 10, curY_ + 3);
    curY_ += layout::kStatusLineH;
}

void Painter::bigNumber(uint32_t value, const char* caption) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)value);
    c_.fillRect(0, curY_, c_.width(), layout::kBigNumberH, kUiBg);
    c_.setTextFont(1);
    c_.setTextDatum(MC_DATUM);
    c_.setTextSize(6);
    c_.setTextColor(kUiFg, kUiBg);
    int numY = curY_ + layout::kBigNumberH / 2 - (caption && caption[0] ? 10 : 0);
    ui::safe_area::drawString(c_, buf, c_.width() / 2, numY);
    if (caption && caption[0]) {
        c_.setTextSize(1);
        c_.setTextColor(kUiMuted, kUiBg);
        ui::safe_area::drawString(c_, caption, c_.width() / 2, numY + 32);
    }
    c_.setTextDatum(TL_DATUM);
    curY_ += layout::kBigNumberH;
}

void Painter::progressBar(uint32_t value, uint32_t max) {
    if (max == 0) max = 1;
    int barW = c_.width() - 48;
    int fillW = (int)((uint64_t)value * barW / max);
    if (fillW < 0) fillW = 0;
    if (fillW > barW) fillW = barW;
    c_.fillRoundRect(24, curY_, barW, layout::kProgressH, 3, kProgressTrack);
    if (fillW > 0)
        c_.fillRoundRect(24, curY_, fillW, layout::kProgressH, 3, kProgressFill);
    curY_ += layout::kProgressPadH;
}

void Painter::clockDisplay(int h, int m, bool highlightH, bool highlightM) {
    c_.fillRect(0, curY_, c_.width(), layout::kClockH, kUiBg);
    c_.setTextFont(1);
    c_.setTextDatum(TL_DATUM);
    c_.setTextSize(4);
    char part[8];
    const auto clock = layout::clockLayout(curY_ + 8, safe_area::applies(c_));
    if (!clock.placed) { curY_ += layout::kClockH; return; }
    snprintf(part, sizeof(part), "%02d", h);
    c_.setTextColor(highlightH ? kUiFg : kUiMuted, kUiBg);
    c_.setCursor(clock.parts[0].x, clock.parts[0].y);
    c_.print(part);
    c_.setTextColor(kUiMuted, kUiBg);
    c_.setCursor(clock.parts[1].x, clock.parts[1].y);
    c_.print(":");
    snprintf(part, sizeof(part), "%02d", m);
    c_.setTextColor(highlightM ? kUiFg : kUiMuted, kUiBg);
    c_.setCursor(clock.parts[2].x, clock.parts[2].y);
    c_.print(part);
    curY_ += layout::kClockH;
}

void Painter::label(const char* text) {
    c_.setTextFont(1);
    c_.setTextDatum(TL_DATUM);
    c_.setTextSize(1);
    c_.setTextColor(kUiFg, kUiBg);
    c_.fillRect(0, curY_, c_.width(), layout::kLabelH, kUiBg);
    ui::safe_area::drawString(c_, text, 12, curY_ + 3);
    curY_ += layout::kLabelH;
}

void Painter::labelCenter(const char* text) {
    c_.setTextFont(1);
    c_.setTextDatum(TC_DATUM);
    c_.setTextSize(1);
    c_.setTextColor(kUiFg, kUiBg);
    c_.fillRect(0, curY_, c_.width(), layout::kLabelH, kUiBg);
    ui::safe_area::drawString(c_, text, c_.width() / 2, curY_ + 3);
    c_.setTextDatum(TL_DATUM);
    curY_ += layout::kLabelH;
}

void Painter::labelMuted(const char* text) {
    c_.setTextFont(1);
    c_.setTextDatum(TL_DATUM);
    c_.setTextSize(1);
    c_.setTextColor(kUiMuted, kUiBg);
    c_.fillRect(0, curY_, c_.width(), layout::kLabelH, kUiBg);
    ui::safe_area::drawString(c_, text, 12, curY_ + 3);
    curY_ += layout::kLabelH;
}

void Painter::labelCenterMuted(const char* text) {
    c_.setTextFont(1);
    c_.setTextDatum(TC_DATUM);
    c_.setTextSize(1);
    c_.setTextColor(kUiMuted, kUiBg);
    c_.fillRect(0, curY_, c_.width(), layout::kLabelH, kUiBg);
    ui::safe_area::drawString(c_, text, c_.width() / 2, curY_ + 3);
    c_.setTextDatum(TL_DATUM);
    curY_ += layout::kLabelH;
}

void Painter::spacer(int pixels) {
    c_.fillRect(0, curY_, c_.width(), pixels, kUiBg);
    curY_ += pixels;
}

void Painter::footer(const char* text) {
    int y = c_.height() - layout::kFooter1H;
    c_.fillRect(0, y, c_.width(), layout::kFooter1H, kUiBg);
    c_.setTextFont(1);
    c_.setTextDatum(TL_DATUM);
    c_.setTextSize(1);
    c_.setTextColor(kUiMuted, kUiBg);
    ui::safe_area::drawString(c_, text, 8, y + 5);
}

void Painter::footerHints(const char* a, const char* b) {
    int y = c_.height() - layout::kFooter2H;
    c_.fillRect(0, y, c_.width(), layout::kFooter2H, kUiBg);
    c_.setTextFont(1);
    c_.setTextDatum(TL_DATUM);
    c_.setTextSize(1);
    c_.setTextColor(kUiMuted, kUiBg);
    ui::safe_area::drawString(c_, a, 12, y + 2);
    ui::safe_area::drawString(c_, b, 12, y + 14);
}

void Painter::footerHints3(const char* a, const char* b, const char* cc) {
    int y = c_.height() - layout::kFooter3H;
    c_.fillRect(0, y, c_.width(), layout::kFooter3H, kUiBg);
    c_.setTextFont(1);
    c_.setTextDatum(TL_DATUM);
    c_.setTextSize(1);
    c_.setTextColor(kUiMuted, kUiBg);
    ui::safe_area::drawString(c_, a, 12, y + 2);
    ui::safe_area::drawString(c_, b, 12, y + 14);
    ui::safe_area::drawString(c_, cc, 12, y + 26);
}

int Painter::width() const { return c_.width(); }

int Painter::listableRows(int rowH, int footerReserveH) const {
    int avail = c_.height() - curY_ - footerReserveH;
    return avail > 0 ? avail / rowH : 0;
}

// ---------------------------------------------------------------------------
// Media / visualiser widgets
// ---------------------------------------------------------------------------

void Painter::verticalBar(int x, int y, int w, int h, int value, int maxValue) {
    c_.fillRect(x - 1, y - 1, w + 2, h + 2, kUiBg);
    c_.fillRoundRect(x, y, w, h, 3, kUiPanel);
    const int fillH = maxValue > 0 ? constrain(value * h / maxValue, 0, h) : 0;
    if (fillH > 0) c_.fillRoundRect(x, y + h - fillH, w, fillH, min(3, fillH / 2), kUiAccent);
}

void Painter::spectrumBars(int x, int y, int barW, int gapPx, int maxH,
                          int nBars, uint8_t* heights, const uint8_t* targetH) {
    for (int b = 0; b < nBars; b++) {
        // targetH values are 0..255 (from the playback spectrum); scale to maxH pixels.
        uint8_t scaled = (uint8_t)((uint32_t)targetH[b] * (uint32_t)maxH / 255u);
        if (scaled > heights[b]) {
            heights[b] = scaled;
        } else if (heights[b] > 2) {
            heights[b] -= 2;
        } else {
            heights[b] = 0;
        }
        int bx   = x + b * (barW + gapPx);
        int barH = heights[b];
        int barY = y + maxH - barH;
        if (barH < maxH) c_.fillRect(bx, y, barW, maxH - barH, kUiBg);
        if (barH > 0) {
            uint16_t col = (barH > maxH * 2 / 3) ? kUiAccent : kUiPanel;
            c_.fillRect(bx, barY, barW, barH, col);
        }
    }
}

void Painter::transportControls(int y, int bandH, bool playing) {
    const int sw = c_.width();
    c_.fillRect(0, y, sw, bandH, kUiBg);
    const int cy = y + bandH / 2;
    const int cx = sw / 2;
    c_.setTextFont(1);
    c_.setTextSize(3);
    c_.setTextColor(kUiFg, kUiBg);
    c_.setTextDatum(MR_DATUM);
    ui::safe_area::drawString(c_, "<<", cx - 52, cy);
    c_.setTextDatum(ML_DATUM);
    ui::safe_area::drawString(c_, ">>", cx + 52, cy);
    c_.setTextDatum(TL_DATUM);
    c_.setTextSize(1);
    const int half = 22;
    c_.fillRect(cx - half, cy - half, 2 * half, 2 * half, kUiBg);
    if (playing) {
        c_.fillRect(cx - 10, cy - 14, 6, 28, kUiFg);
        c_.fillRect(cx + 4,  cy - 14, 6, 28, kUiFg);
    } else {
        c_.fillTriangle(cx - 9, cy - 16, cx - 9, cy + 16, cx + 18, cy, kUiFg);
    }
}

int Painter::chunkedProgress(int x, int y, int w, int h,
                             uint32_t elapsedMs, uint32_t totalMs,
                             int prevFilledPx, bool drawBorder) {
    int filled = 0;
    if (totalMs > 0) {
        if (elapsedMs >= totalMs) {
            filled = w;
        } else {
            filled = (int)((int64_t)w * (int64_t)elapsedMs / (int64_t)totalMs);
        }
    }
    const int chunk = 4;
    if (drawBorder) {
        c_.fillRect(x, y, w, h, kUiPanel);
        for (int xi = 0; xi + chunk <= filled; xi += chunk)
            c_.fillRect(x + xi, y + 1, chunk - 1, h - 2, kUiAccent);
        c_.drawRect(x - 1, y - 1, w + 2, h + 2, kUiMuted);
    } else {
        // Incremental: only paint newly filled chunks.
        for (int xi = prevFilledPx; xi + chunk <= filled; xi += chunk)
            c_.fillRect(x + xi, y + 1, chunk - 1, h - 2, kUiAccent);
    }
    return filled;
}

void Painter::progressTimes(int x, int y, int w, uint32_t elapsedMs, uint32_t totalMs) {
    c_.fillRect(x, y, w, 12, kUiBg);
    char t1[12], t2[12];
    formatDurationMs(elapsedMs, t1, sizeof(t1));
    formatDurationMs(totalMs > 0 ? totalMs : 0, t2, sizeof(t2));
    c_.setTextSize(1);
    c_.setTextFont(1);
    c_.setTextDatum(TL_DATUM);
    c_.setTextColor(kUiMuted, kUiBg);
    ui::safe_area::drawString(c_, t1, x, y);
    c_.setTextDatum(TR_DATUM);
    c_.setTextColor(kUiFg, kUiBg);
    ui::safe_area::drawString(c_, t2, x + w, y);
    c_.setTextDatum(TL_DATUM);
}

void formatDurationMs(uint32_t ms, char* out, size_t cap) {
    uint32_t s = ms / 1000u;
    snprintf(out, cap, "%u:%02u", (unsigned)(s / 60u), (unsigned)(s % 60u));
}

void ellipsizedLabel(TFT_eSPI& c, const char* text, int x, int y,
                     int maxPx, uint8_t font, uint16_t fg, uint16_t bg) {
    if (!text) return;
    c.setTextFont(font);
    c.setTextColor(fg, bg);
    safe_area::drawLabel(c,text,{x,y,maxPx,c.fontHeight(font)},TL_DATUM,font);
}

}  // namespace ui
