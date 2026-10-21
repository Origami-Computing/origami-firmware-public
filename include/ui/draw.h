#pragma once

#include <TB_TFT_eSPI.h>
#include <stdint.h>
#include "ui/layout_metrics.h"
#include "ui/list_row.h"

namespace ui {

/** Centered "Opening <app>…" splash — painted before a slow app onEnter(). */
void paintAppOpeningSplash(TFT_eSprite& canvas, const char* appName);

/** Paint opening splash, flush canvas, and repaint status chrome (Camera, Music, …). */
void showAppOpeningSplash(const char* appName);

// Format milliseconds as "M:SS" (e.g. 65000 -> "1:05").
void formatDurationMs(uint32_t ms, char* out, size_t cap);

// Print text into [x, x+maxPx) using given TFT font, truncating with "..." if needed.
void ellipsizedLabel(TFT_eSPI& c, const char* text, int x, int y,
                     int maxPx, uint8_t font, uint16_t fg, uint16_t bg);

struct Rect {
    int16_t x, y, w, h;
};

inline Rect splitTop(Rect r, int16_t height) {
    return {r.x, r.y, r.w, height};
}

inline Rect splitBottom(Rect r, int16_t height) {
    return {r.x, (int16_t)(r.y + r.h - height), r.w, height};
}

inline Rect inset(Rect r, int16_t dx, int16_t dy) {
    return {(int16_t)(r.x + dx), (int16_t)(r.y + dy),
            (int16_t)(r.w - 2 * dx), (int16_t)(r.h - 2 * dy)};
}

inline Rect contentArea(int16_t screenW, int16_t screenH, int16_t statusBarH) {
    return {0, statusBarH, screenW, (int16_t)(screenH - statusBarH)};
}

// Low-level primitives (used by Painter internally and by custom renderers).
void fill(TFT_eSPI& c, Rect r, uint16_t color);
/** Render one list row with an iPod-style right-side indicator.
 *  choice=false (default): navigation row — always shows '>'.
 *  choice=true, checked=true: radio row — shows a filled dot '●'.
 *  choice=true, checked=false: radio row — no indicator (empty right zone). */
void listRow(TFT_eSPI& c, int x, int y, int w, int h,
             const char* text, bool selected,
             uint16_t selBg, uint16_t selFg, uint16_t bg, uint16_t fg,
             bool choice = false, bool checked = false,
             uint16_t indicatorFg = 0, bool opensScreen = true);
// Standard row metadata, including editing/blink state, shared with ListView.
void listRow(TFT_eSPI& c, int x, int y, int w, int h,
             const ListItem& item, bool selected,
             uint16_t selBg, uint16_t selFg, uint16_t bg);
void scrollbar(TFT_eSPI& c, int x, int y, int trackH, int trackW,
               int visibleRows, int totalItems, int scrollFirst,
               uint16_t trackColor, uint16_t thumbColor);

// ---------------------------------------------------------------------------
// Painter — theme-aware, coordinate-free UI builder.
//
// Immediate drawing helpers for views that use an existing TFT surface.
// This is not scene::Screen (lifecycle) or ui::scene::Scene (node storage).
// Node-based views use ui::scene::Builder; both share layout metrics.
//
// Usage:
//   void onDraw(TFT_eSprite& canvas) {
//     ui::Painter s(canvas);
//     s.clear();
//     s.panelHeader("Settings");
//     s.listRow("WiFi Setup", sel == 0);
//     s.listRow("About",      sel == 1);
//     s.footer("SCROLL  A=open  B=back");
//   }
// ---------------------------------------------------------------------------
class Painter {
public:
    explicit Painter(TFT_eSPI& c);

    // --- Layout entry points ---
    void clear();                            // fill bg, reset cursor to content top
    void panelHeader(const char* text);      // grey title strip, advance cursor

    // --- Flowing rows (each call advances the internal Y cursor) ---
    void listRow(const char* text, bool selected, bool marked = false);
    void onOffRow(const char* label, bool on, bool selected);
    void emptyState(const char* line1, const char* line2 = nullptr);

    // --- Status lines (semantic colours) ---
    void statusLine(const char* text);       // normal foreground
    void statusLineOk(const char* text);     // blue  (connected / success)
    void statusLineMuted(const char* text);  // dimmed (inactive info)

    // --- Data widgets ---
    void bigNumber(uint32_t value, const char* caption);
    void progressBar(uint32_t value, uint32_t max);
    void clockDisplay(int h, int m, bool highlightH, bool highlightM);

    // --- Generic text (left / centred, normal / muted) ---
    void label(const char* text);
    void labelCenter(const char* text);
    void labelMuted(const char* text);
    void labelCenterMuted(const char* text);
    void spacer(int pixels = layout::space(2));

    // --- Footer (fixed at bottom, does not advance the cursor) ---
    void footer(const char* text);           // single line
    void footerHints(const char* a, const char* b);      // two lines
    void footerHints3(const char* a, const char* b, const char* c);  // three lines

    // --- Geometry helpers ---
    int curY()  const { return curY_; }
    int width() const;
    // Rows that fit between curY_ and the footer.
    // footerReserveH: pass 22/28/44 matching which footer variant you'll use.
    int listableRows(int rowH, int footerReserveH) const;

    // --- Media / visualiser widgets (direct-panel safe: work on any TFT_eSPI) ---

    // Vertical bar fill (e.g. volume): draws a bordered rectangle with accent
    // fill proportional to value/maxValue, bottom-aligned.
    void verticalBar(int x, int y, int w, int h, int value, int maxValue);

    // Spectrum analyser bars: nBars columns of barW px separated by gapPx.
    // heights[b] is current bar height (0..maxH); targetH[b] is the new
    // peak (fast-attack / 2-unit-per-tick slow-release applied inside).
    // Updates heights[] in-place.  Call at ~50 ms cadence.
    void spectrumBars(int x, int y, int barW, int gapPx, int maxH,
                      int nBars, uint8_t* heights, const uint8_t* targetH);

    // Transport row: pause/play icon centred + "<<" and ">>" either side.
    // playing == true draws the ‖ pause glyph; false draws the ▶ play glyph.
    void transportControls(int y, int bandH, bool playing);

    // Chunked progress bar (accent filled in 4-px chunks with gaps).
    // Pass prevFilledPx to do incremental updates (only draws new chunks).
    // Returns new filledPx so caller can store it.
    // Pass drawBorder=true on full redraws (clears track first).
    int chunkedProgress(int x, int y, int w, int h,
                         uint32_t elapsedMs, uint32_t totalMs,
                         int prevFilledPx, bool drawBorder);

    // Elapsed / total time labels below a progress bar.
    void progressTimes(int x, int y, int w, uint32_t elapsedMs, uint32_t totalMs);

    // --- Escape hatch: custom game rendering only ---
    TFT_eSPI& raw() { return c_; }

private:
    TFT_eSPI& c_;
    int curY_;
};

}  // namespace ui
