#pragma once
// Photos gallery chrome: full-bleed media with on-demand overlays.
//
// Shared by src/apps/photos_app.cpp (TFT_eSprite canvas) and the host design
// study tools/mp3-preview/photos_study.h, so the preview and the device draw
// the same pixels. C must provide readPixel(x, y), drawPixel(x, y, color) and
// fillRect(x, y, w, h, color) in RGB565. Every primitive clips to W x H.
//
// Overlays are designed for partial repaint over the decoded photo, so moving
// focus never re-decodes a JPEG: the sheet's row area and the confirm buttons
// are opaque and can be refreshed in place (full == false).
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <algorithm>
#include "ui/proportional_text.h"

namespace ui {
namespace photos_view {

constexpr uint16_t rgb(int r, int g, int b) {
    return uint16_t(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
constexpr uint16_t kBlack = 0x0000, kWhite = 0xFFFF;
constexpr uint16_t kMuted = rgb(150, 150, 150);
constexpr uint16_t kSheet = rgb(22, 22, 24);
constexpr uint16_t kRaised = rgb(48, 48, 52);
constexpr uint16_t kTrack = rgb(70, 70, 74);
constexpr uint16_t kGrabber = rgb(80, 80, 84);
constexpr uint16_t kRed = rgb(255, 82, 72);
constexpr uint16_t kGreen = rgb(80, 210, 120);
constexpr uint16_t kIconMuted = rgb(110, 110, 116);

inline uint16_t mix(uint16_t fg, uint16_t bg, int a) {
    if (a >= 255) return fg;
    if (a <= 0) return bg;
    const int r = (((fg >> 11) & 31) * a + ((bg >> 11) & 31) * (255 - a) + 127) / 255;
    const int g = (((fg >> 5) & 63) * a + ((bg >> 5) & 63) * (255 - a) + 127) / 255;
    const int b = ((fg & 31) * a + (bg & 31) * (255 - a) + 127) / 255;
    return uint16_t((r << 11) | (g << 5) | b);
}

struct Bounds { int w, h; };

template <class C> void blend(C& c, Bounds s, int x, int y, uint16_t color, int a) {
    if (x < 0 || y < 0 || x >= s.w || y >= s.h || a <= 0) return;
    c.drawPixel(x, y, a >= 255 ? color : mix(color, c.readPixel(x, y), a));
}
template <class C> void fill(C& c, Bounds s, int x, int y, int w, int h, uint16_t color) {
    const int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(s.w, x + w), y1 = std::min(s.h, y + h);
    if (x1 > x0 && y1 > y0) c.fillRect(x0, y0, x1 - x0, y1 - y0, color);
}

// Darkens everything already drawn (a scrim). keep = 0..255 brightness retained.
template <class C> void dim(C& c, Bounds s, int keep) {
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x) c.drawPixel(x, y, mix(c.readPixel(x, y), kBlack, keep));
}

// Anti-aliased rounded rectangle. Opaque interiors use fillRect; only corner
// pixels are blended.
template <class C> void roundRect(C& c, Bounds s, int x, int y, int w, int h, int r, uint16_t color,
                                  int alpha = 255, bool topOnly = false) {
    r = std::min(r, std::min(w, h) / 2);
    const int bottomR = topOnly ? 0 : r;
    for (int yy = 0; yy < h; ++yy) {
        const bool cornerRow = yy < r || yy >= h - bottomR;
        if (!cornerRow) {
            if (alpha >= 255) fill(c, s, x, y + yy, w, 1, color);
            else for (int xx = 0; xx < w; ++xx) blend(c, s, x + xx, y + yy, color, alpha);
            continue;
        }
        const float dy = yy < r ? r - yy - 0.5f : yy - (h - bottomR) + 0.5f;
        for (int xx = 0; xx < w; ++xx) {
            const float dx = xx < r ? r - xx - 0.5f : xx >= w - r ? xx - (w - r) + 0.5f : 0.f;
            int cov = 255;
            if (dx > 0) cov = int(std::min(1.f, std::max(0.f, r - sqrtf(dx * dx + dy * dy))) * 255);
            blend(c, s, x + xx, y + yy, color, cov * alpha / 255);
        }
    }
}

template <class C> void disc(C& c, Bounds s, float cx, float cy, float r, uint16_t color, int alpha = 255) {
    for (int y = int(cy - r - 1); y <= int(cy + r + 1); ++y)
        for (int x = int(cx - r - 1); x <= int(cx + r + 1); ++x) {
            const float d = hypotf(x + 0.5f - cx, y + 0.5f - cy) - r;
            blend(c, s, x, y, color, int(std::min(1.f, std::max(0.f, 0.5f - d)) * alpha));
        }
}

template <class C> void ring(C& c, Bounds s, float cx, float cy, float r, float t, uint16_t color) {
    for (int y = int(cy - r - 2); y <= int(cy + r + 2); ++y)
        for (int x = int(cx - r - 2); x <= int(cx + r + 2); ++x) {
            const float d = fabsf(hypotf(x + 0.5f - cx, y + 0.5f - cy) - r) - t / 2;
            blend(c, s, x, y, color, int(std::min(1.f, std::max(0.f, 0.5f - d)) * 255));
        }
}

// Filled triangle, 4x4 supersampled.
template <class C> void triangle(C& c, Bounds s, float x0, float y0, float x1, float y1, float x2, float y2,
                                 uint16_t color) {
    auto edge = [](float ax, float ay, float bx, float by, float px, float py) {
        return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
    };
    const float area = edge(x0, y0, x1, y1, x2, y2);
    const int minx = int(std::min(x0, std::min(x1, x2))) - 1, maxx = int(std::max(x0, std::max(x1, x2))) + 1;
    const int miny = int(std::min(y0, std::min(y1, y2))) - 1, maxy = int(std::max(y0, std::max(y1, y2))) + 1;
    for (int y = miny; y <= maxy; ++y)
        for (int x = minx; x <= maxx; ++x) {
            int hits = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    const float px = x + (sx + 0.5f) / 4, py = y + (sy + 0.5f) / 4;
                    const float a = edge(x1, y1, x2, y2, px, py), b = edge(x2, y2, x0, y0, px, py),
                                d = edge(x0, y0, x1, y1, px, py);
                    if (area > 0 ? (a >= 0 && b >= 0 && d >= 0) : (a <= 0 && b <= 0 && d <= 0)) ++hits;
                }
            blend(c, s, x, y, color, hits * 255 / 16);
        }
}

// Thick anti-aliased line with round caps.
template <class C> void line(C& c, Bounds s, float x0, float y0, float x1, float y1, float t, uint16_t color) {
    const float vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy;
    for (int y = int(std::min(y0, y1) - t); y <= int(std::max(y0, y1) + t); ++y)
        for (int x = int(std::min(x0, x1) - t); x <= int(std::max(x0, x1) + t); ++x) {
            const float px = x + 0.5f - x0, py = y + 0.5f - y0;
            const float k = len2 > 0 ? std::min(1.f, std::max(0.f, (px * vx + py * vy) / len2)) : 0.f;
            const float d = hypotf(px - k * vx, py - k * vy) - t / 2;
            blend(c, s, x, y, color, int(std::min(1.f, std::max(0.f, 0.5f - d)) * 255));
        }
}

// --- Text (Instrument, blended against the pixels underneath) ---------------

template <class C> void text(C& c, Bounds s, const char* str, int x, int y, int px, uint16_t ink) {
    const auto& f = proportional::font(px);
    const int baseline = y - proportional::top(str, px);
    unsigned advance = 0;
    for (; *str; ++str) {
        const auto& g = proportional::glyph(*str, px);
        const int left = x + int((advance + 32) / 64) + g.x;
        for (int row = 0; row < g.h; ++row)
            for (int col = 0; col < g.w; ++col)
                blend(c, s, left + col, baseline + g.y + row, ink, f.pixels[g.offset + row * g.w + col]);
        advance += g.advance;
    }
}
inline int textWidth(const char* str, int px) { return proportional::width(str, px); }
inline int capHeight(int px) { return proportional::height("B", px); }
template <class C> void textMiddle(C& c, Bounds s, const char* str, int x, int y, int h, int px, uint16_t ink) {
    text(c, s, str, x, y + (h - capHeight(px)) / 2, px, ink);
}
template <class C> void textCenter(C& c, Bounds s, const char* str, int cx, int y, int px, uint16_t ink) {
    text(c, s, str, cx - textWidth(str, px) / 2, y, px, ink);
}

// --- Icons ------------------------------------------------------------------

template <class C> void iconPlay(C& c, Bounds s, float cx, float cy, float size, uint16_t ink) {
    triangle(c, s, cx - size * 0.38f, cy - size * 0.5f, cx - size * 0.38f, cy + size * 0.5f, cx + size * 0.5f, cy, ink);
}
template <class C> void iconUpload(C& c, Bounds s, float cx, float cy, uint16_t ink) {
    line(c, s, cx, cy + 7, cx, cy - 6, 2.2f, ink);
    line(c, s, cx - 5.5f, cy - 1, cx, cy - 6.5f, 2.2f, ink);
    line(c, s, cx + 5.5f, cy - 1, cx, cy - 6.5f, 2.2f, ink);
    line(c, s, cx - 8, cy + 8, cx + 8, cy + 8, 2.0f, ink);
}
template <class C> void iconTrash(C& c, Bounds s, float cx, float cy, uint16_t ink) {
    line(c, s, cx - 8, cy - 5, cx + 8, cy - 5, 2.0f, ink);
    line(c, s, cx - 3, cy - 8, cx + 3, cy - 8, 2.0f, ink);
    line(c, s, cx - 6, cy - 4, cx - 5, cy + 8, 2.0f, ink);
    line(c, s, cx + 6, cy - 4, cx + 5, cy + 8, 2.0f, ink);
    line(c, s, cx - 5, cy + 8, cx + 5, cy + 8, 2.0f, ink);
    line(c, s, cx - 1.5f, cy - 1, cx - 1.5f, cy + 5, 1.6f, ink);
    line(c, s, cx + 1.5f, cy - 1, cx + 1.5f, cy + 5, 1.6f, ink);
}
template <class C> void iconCheck(C& c, Bounds s, float cx, float cy, float size, uint16_t ink, float t = 2.2f) {
    line(c, s, cx - size * 0.5f, cy, cx - size * 0.15f, cy + size * 0.35f, t, ink);
    line(c, s, cx - size * 0.15f, cy + size * 0.35f, cx + size * 0.5f, cy - size * 0.35f, t, ink);
}
template <class C> void iconPicture(C& c, Bounds s, int x, int y, int w, int h, uint16_t ink) {
    roundRect(c, s, x, y, w, h, 6, ink);
    roundRect(c, s, x + 3, y + 3, w - 6, h - 6, 4, kBlack);
    disc(c, s, x + w * 0.68f, y + h * 0.34f, 4.5f, ink);
    triangle(c, s, x + 8, y + h - 8, x + w * 0.38f, y + h * 0.4f, x + w * 0.66f, y + h - 8, ink);
    triangle(c, s, x + w * 0.5f, y + h - 8, x + w * 0.7f, y + h * 0.58f, x + w - 8, y + h - 8, ink);
}

// --- Browsing ---------------------------------------------------------------

// The decoded picture's vertical extent, found from the black letterbox the
// gallery clears before decoding. Sparse sampling; a near-black photo edge may
// read as letterbox, which only moves the readout onto that dark edge.
struct ImageRows { int top, bottom; };
template <class C> ImageRows findImageRows(C& c, Bounds s) {
    auto blank = [&](int y) {
        for (int x = 2; x < s.w; x += 6) if (c.readPixel(x, y) != kBlack) return false;
        return true;
    };
    ImageRows r{0, s.h};
    while (r.top < s.h / 2 && blank(r.top)) ++r.top;
    while (r.bottom > s.h / 2 && blank(r.bottom - 1)) --r.bottom;
    return r;
}

template <class C> void playBadge(C& c, Bounds s, int cx, int cy) {
    disc(c, s, cx, cy, 25, kBlack, 150);
    ring(c, s, cx, cy, 24, 2.0f, kWhite);
    iconPlay(c, s, cx + 2, cy, 20, kWhite);
}

template <class C> void durationChip(C& c, Bounds s, const char* str, int right, int bottom) {
    const int w = textWidth(str, 12) + 16, h = 20;
    roundRect(c, s, right - w, bottom - h, w, h, 10, kBlack, 170);
    textMiddle(c, s, str, right - w + 8, bottom - h, h, 12, kWhite);
}

// Minimum letterbox height that can hold the readout without covering media.
constexpr int kReadoutBarMin = 14;

// Position readout + scrubber. Drawn in the letterbox below the picture when
// there is room (cleared later with one fillRect); otherwise on a chip.
template <class C> void positionReadout(C& c, Bounds s, int index1, int count, int barY) {
    char buf[24];
    snprintf(buf, sizeof buf, "%d  of  %d", index1, count);
    const int trackW = 64;
    const bool inBar = s.h - barY >= kReadoutBarMin;
    const int y = inBar ? barY : s.h - 30, h = inBar ? s.h - barY : 22;
    // Centred as one group: the rev-4 opening has 39 px corner radii that
    // hide anything near the bottom corners.
    const int gap = 12, textW = textWidth(buf, 12), groupW = textW + gap + trackW;
    const int gx = (s.w - groupW) / 2;
    if (!inBar) roundRect(c, s, gx - 12, y, groupW + 24, h, 11, kBlack, 170);
    textMiddle(c, s, buf, gx, y, h, 12, kMuted);
    const int tx = gx + textW + gap, ty = y + h / 2 - 1;
    roundRect(c, s, tx, ty, trackW, 3, 1, kTrack);
    const int thumb = std::max(6, trackW / std::max(1, count));
    const int pos = count > 1 ? (trackW - thumb) * (index1 - 1) / (count - 1) : 0;
    roundRect(c, s, tx + pos, ty, thumb, 3, 1, kWhite);
}

// --- Full-screen captions ---------------------------------------------------
// Layers over cover-cropped media: an index pill at the top and the capture
// date/time at the bottom. Both stay clear of the opening's rounded corners.

constexpr int kIndexPillTop = 12, kIndexPillH = 26, kCaptionSize = 16;

// "2 / 26" in a translucent pill, top centre.
template <class C> void indexPill(C& c, Bounds s, int index1, int count) {
    char buf[24];
    snprintf(buf, sizeof buf, "%d / %d", index1, count);
    const int w = textWidth(buf, kCaptionSize) + 28;
    roundRect(c, s, (s.w - w) / 2, kIndexPillTop, w, kIndexPillH, kIndexPillH / 2, kBlack, 140);
    textMiddle(c, s, buf, (s.w - textWidth(buf, kCaptionSize)) / 2, kIndexPillTop, kIndexPillH, kCaptionSize, kWhite);
}

// Soft dark gradient (not used by the gallery: captions use a text shadow).
template <class C> void bottomScrim(C& c, Bounds s, int height, int maxAlpha) {
    for (int i = 0; i < height; ++i) {
        const int y = s.h - height + i, a = maxAlpha * (i + 1) / height;
        for (int x = 0; x < s.w; ++x) blend(c, s, x, y, kBlack, a);
    }
}

// White text with a 1 px dark drop shadow: legible on bright media without
// shading the picture itself.
template <class C> void shadowText(C& c, Bounds s, const char* str, int x, int y, int px, uint16_t ink = kWhite) {
    text(c, s, str, x + 1, y + 1, px, kBlack);
    text(c, s, str, x, y, px, ink);
}

// Capture date and time, bottom centre: "24 Nov · 4:12 pm". Segments are
// joined by a drawn middle dot (the UI font is ASCII-only). Null segments are
// skipped; pass all null when the photo has no capture time and nothing draws.
template <class C> void dateCaption(C& c, Bounds s, const char* const* parts, int n) {
    int count = 0, width = 0;
    const int gap = 14;  // space for " · "
    for (int i = 0; i < n; ++i) if (parts[i] && *parts[i]) { width += textWidth(parts[i], kCaptionSize); ++count; }
    if (!count) return;
    width += gap * (count - 1);
    const int y = s.h - 14 - capHeight(kCaptionSize);  // no gradient: text shadow only
    int x = (s.w - width) / 2, drawn = 0;
    for (int i = 0; i < n; ++i) {
        if (!parts[i] || !*parts[i]) continue;
        if (drawn++) {
            const float cy = y + capHeight(kCaptionSize) / 2.f;
            disc(c, s, x + gap / 2.f + 1, cy + 1, 1.6f, kBlack);
            disc(c, s, x + gap / 2.f, cy, 1.6f, kWhite);
            x += gap;
        }
        shadowText(c, s, parts[i], x, y, kCaptionSize);
        x += textWidth(parts[i], kCaptionSize);
    }
}

// Formats a capture time (local, already offset) as {"24 Nov", "4:12 pm"}.
// Adds the year when it differs from the current one. Pure; host-testable.
struct CaptureText { char date[16]; char time[12]; };
inline CaptureText formatCapture(int year, int month, int day, int hour, int minute, int currentYear) {
    static const char* const kMonths[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    CaptureText t{};
    const char* mon = kMonths[(month >= 1 && month <= 12) ? month - 1 : 0];
    if (year != currentYear) snprintf(t.date, sizeof t.date, "%d %s %d", day, mon, year);
    else snprintf(t.date, sizeof t.date, "%d %s", day, mon);
    const int h12 = hour % 12 == 0 ? 12 : hour % 12;
    snprintf(t.time, sizeof t.time, "%d:%02d %s", h12, minute, hour < 12 ? "am" : "pm");
    return t;
}

// --- Video title card -------------------------------------------------------
// Bottom-left card over the playing video: name, a status line with the time
// on the right, and a progress track with a knob. It shows when a video
// starts, fades out shortly after, and returns whenever playback pauses or
// ends. No gradient: text and track carry their own shadow.

// Draws through to C with every write faded by `alpha` (0..255), so one card
// painter serves both the solid and the fading frames.
template <class C> struct Faded {
    C& c; int alpha;
    uint16_t readPixel(int x, int y) const { return c.readPixel(x, y); }
    void drawPixel(int x, int y, uint16_t color) { c.drawPixel(x, y, mix(color, c.readPixel(x, y), alpha)); }
    void fillRect(int x, int y, int w, int h, uint16_t color) {
        for (int yy = y; yy < y + h; ++yy) for (int xx = x; xx < x + w; ++xx) drawPixel(xx, yy, color);
    }
};

struct VideoCard {
    const char* name;      // "VID_0042"
    const char* status;    // "Starting video" / "Paused" / "Ended"
    const char* time;      // "0:14" or "0:05 / 0:14"
    unsigned progress;     // 0..1000
};
constexpr int kCardLeft = 24, kCardRight = 256, kCardTrackY = 212;
constexpr uint16_t kCardSubtle = rgb(210, 210, 210);

template <class C> void videoCardSolid(C& c, Bounds s, const VideoCard& v) {
    const int nameY = kCardTrackY - 50, lineY = kCardTrackY - 24;
    shadowText(c, s, v.name, kCardLeft, nameY, 20);
    shadowText(c, s, v.status, kCardLeft, lineY, 14, kCardSubtle);
    if (v.time && *v.time) shadowText(c, s, v.time, kCardRight - textWidth(v.time, 14), lineY, 14, kCardSubtle);
    const int w = kCardRight - kCardLeft, p = v.progress > 1000 ? 1000 : int(v.progress);
    const int fill = w * p / 1000;
    roundRect(c, s, kCardLeft, kCardTrackY + 1, w, 4, 2, kBlack, 140);            // shadow
    roundRect(c, s, kCardLeft, kCardTrackY, w, 4, 2, rgb(120, 120, 124));          // track
    if (fill > 0) roundRect(c, s, kCardLeft, kCardTrackY, fill, 4, 2, kWhite);     // elapsed
    disc(c, s, kCardLeft + fill + 0.5f, kCardTrackY + 3, 6, kBlack, 120);          // knob shadow
    disc(c, s, kCardLeft + fill + 0.5f, kCardTrackY + 2, 6, kWhite);               // knob
}

template <class C> void videoCard(C& c, Bounds s, const VideoCard& v, int alpha) {
    if (alpha <= 0) return;
    if (alpha >= 255) { videoCardSolid(c, s, v); return; }
    Faded<C> faded{c, alpha};
    videoCardSolid(faded, s, v);
}

// Card opacity for a clip: solid while paused/ended or for kCardHoldMs after
// playback (re)starts, then a kCardFadeMs fade to nothing.
constexpr int kCardHoldMs = 900, kCardFadeMs = 350;
inline int videoCardAlpha(bool playing, uint32_t msSincePlay) {
    if (!playing || msSincePlay < uint32_t(kCardHoldMs)) return 255;
    const uint32_t t = msSincePlay - kCardHoldMs;
    return t >= uint32_t(kCardFadeMs) ? 0 : int(255 - t * 255 / kCardFadeMs);
}

// --- Action sheet -----------------------------------------------------------

enum class Icon : uint8_t { Play, Upload, Trash };
struct SheetRow { const char* label; Icon icon; bool danger; };
struct SheetModel {
    const char* kind;        // "Photo" / "Video"
    int index1, count;
    bool uploaded;
    const char* error;       // replaces the cloud state when set
    const SheetRow* rows;
    int rowCount, selected;
};
constexpr int kSheetRowH = 34, kSheetHeader = 30;
inline int sheetTop(Bounds s, int rowCount) { return s.h - (kSheetHeader + rowCount * kSheetRowH + 14); }

template <class C> void sheetRows(C& c, Bounds s, const SheetModel& m) {
    const int y0 = sheetTop(s, m.rowCount) + kSheetHeader;
    fill(c, s, 0, y0, s.w, m.rowCount * kSheetRowH, kSheet);
    for (int i = 0; i < m.rowCount; ++i) {
        const auto& row = m.rows[i];
        const int y = y0 + i * kSheetRowH;
        uint16_t ink = row.danger ? kRed : kWhite;
        if (i == m.selected) {
            roundRect(c, s, 10, y + 2, s.w - 20, kSheetRowH - 4, 10, row.danger ? kRed : kWhite);
            ink = row.danger ? kWhite : kBlack;
        }
        const float cy = y + kSheetRowH / 2.f;
        if (row.icon == Icon::Play) iconPlay(c, s, 35, cy, 15, ink);
        else if (row.icon == Icon::Upload) iconUpload(c, s, 34, cy, ink);
        else iconTrash(c, s, 34, cy, ink);
        textMiddle(c, s, row.label, 56, y, kSheetRowH, 18, ink);
    }
}

template <class C> void sheet(C& c, Bounds s, const SheetModel& m, bool full) {
    if (!full) { sheetRows(c, s, m); return; }
    const int y0 = sheetTop(s, m.rowCount);
    dim(c, s, 110);
    roundRect(c, s, 0, y0, s.w, s.h - y0, 14, kSheet, 255, true);
    roundRect(c, s, s.w / 2 - 16, y0 + 6, 32, 3, 1, kGrabber);
    char title[32];
    snprintf(title, sizeof title, "%s    %d  of  %d", m.kind, m.index1, m.count);
    text(c, s, title, 18, y0 + 15, 12, kMuted);
    const char* state = m.error ? m.error : m.uploaded ? "Uploaded" : "Not  uploaded";
    const uint16_t ink = m.error ? kRed : m.uploaded ? kGreen : kMuted;
    const int sw = textWidth(state, 12);
    if (m.uploaded && !m.error) iconCheck(c, s, s.w - 18 - sw - 9, y0 + 19.5f, 9, kGreen, 1.8f);
    text(c, s, state, s.w - 18 - sw, y0 + 15, 12, ink);
    sheetRows(c, s, m);
}

// --- Delete confirmation ----------------------------------------------------

template <class C> void confirmButtons(C& c, Bounds s, bool deleteFocused) {
    const int cw = 232, ch = 128, x = (s.w - cw) / 2, y = (s.h - ch) / 2;
    const int bw = 100, bh = 30, by = y + ch - bh - 12, bx0 = x + 12, bx1 = x + cw - 12 - bw;
    fill(c, s, x + 8, by, cw - 16, bh, kSheet);
    const int ty = by + (bh - capHeight(18)) / 2;
    roundRect(c, s, bx0, by, bw, bh, 10, deleteFocused ? kRaised : kWhite);
    textCenter(c, s, "Cancel", bx0 + bw / 2, ty, 18, deleteFocused ? kWhite : kBlack);
    roundRect(c, s, bx1, by, bw, bh, 10, deleteFocused ? kRed : kRaised);
    textCenter(c, s, "Delete", bx1 + bw / 2, ty, 18, deleteFocused ? kWhite : kRed);
}

template <class C> void confirmDelete(C& c, Bounds s, bool video, bool deleteFocused, bool full) {
    if (!full) { confirmButtons(c, s, deleteFocused); return; }
    const int cw = 232, ch = 128, x = (s.w - cw) / 2, y = (s.h - ch) / 2;
    dim(c, s, 90);
    roundRect(c, s, x, y, cw, ch, 14, kSheet);
    iconTrash(c, s, s.w / 2, y + 24, kRed);
    textCenter(c, s, video ? "Delete this video?" : "Delete this photo?", s.w / 2, y + 42, 18, kWhite);
    textCenter(c, s, "It  will  be  removed  from  this  device.", s.w / 2, y + 66, 12, kMuted);
    confirmButtons(c, s, deleteFocused);
}

// --- Notice ---------------------------------------------------------------

// Brief confirmation pill near the bottom edge, e.g. "Added to upload queue".
// Shown over the photo for a moment; the caller redraws the image to clear it.
constexpr int kNoticeH = 30;
template <class C> void notice(C& c, Bounds s, const char* label, bool ok) {
    const int iconW = 22, w = textWidth(label, 12) + iconW + 26, x = (s.w - w) / 2, y = s.h - kNoticeH - 12;
    roundRect(c, s, x, y, w, kNoticeH, kNoticeH / 2, kSheet, 235);
    const float cx = x + 13 + iconW / 2.f - 4, cy = y + kNoticeH / 2.f;
    disc(c, s, cx, cy, 8, ok ? kGreen : kRed);
    if (ok) iconCheck(c, s, cx, cy + 0.5f, 8, kBlack, 2.0f);
    else { line(c, s, cx - 3, cy - 3, cx + 3, cy + 3, 2.0f, kBlack); line(c, s, cx + 3, cy - 3, cx - 3, cy + 3, 2.0f, kBlack); }
    textMiddle(c, s, label, x + 13 + iconW, y, kNoticeH, 12, kWhite);
}

// --- Empty / error ----------------------------------------------------------

template <class C> void message(C& c, Bounds s, const char* title, const char* line1, const char* line2) {
    c.fillRect(0, 0, s.w, s.h, kBlack);
    const int top = s.h / 2 - 60;
    iconPicture(c, s, s.w / 2 - 28, top, 56, 46, kIconMuted);
    textCenter(c, s, title, s.w / 2, top + 68, 18, kWhite);
    if (line1) textCenter(c, s, line1, s.w / 2, top + 96, 12, kMuted);
    if (line2) textCenter(c, s, line2, s.w / 2, top + 112, 12, kMuted);
}

}  // namespace photos_view
}  // namespace ui
