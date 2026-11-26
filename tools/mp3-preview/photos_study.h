#pragma once
// Photos app design study (host only). Renders the current staging layout and
// the proposed full-bleed redesign at native 280x240 so they can be compared
// before porting. Text uses the firmware's own font data: the GLCD 5x7 font
// that scene labels fall back to today, and the Instrument face used elsewhere.
// Sample images are procedural placeholders, not real captures.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "ui/proportional_text.h"
#include "ui/photos_view.h"
#include "pgmspace.h"
#include "../../lib/TB_TFT_eSPI/Fonts/glcdfont.c"

namespace photos_study {

constexpr int W = 280, H = 240;
constexpr int kStatusBarH = 38;  // include/config/display_safe_area.h

inline uint16_t rgb(int r, int g, int b) {
    r = std::clamp(r, 0, 255); g = std::clamp(g, 0, 255); b = std::clamp(b, 0, 255);
    return uint16_t(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
inline void unpack(uint16_t c, int& r, int& g, int& b) {
    r = ((c >> 11) & 31) * 255 / 31; g = ((c >> 5) & 63) * 255 / 63; b = (c & 31) * 255 / 31;
}
inline uint16_t mix(uint16_t fg, uint16_t bg, int a /*0..255*/) {
    int r1, g1, b1, r2, g2, b2; unpack(fg, r1, g1, b1); unpack(bg, r2, g2, b2);
    return rgb((r1 * a + r2 * (255 - a)) / 255, (g1 * a + g2 * (255 - a)) / 255, (b1 * a + b2 * (255 - a)) / 255);
}

constexpr uint16_t kBlack = 0x0000, kWhite = 0xFFFF;
const uint16_t kMuted = rgb(150, 150, 150);
const uint16_t kSheet = rgb(22, 22, 24);
const uint16_t kRed = rgb(255, 82, 72);
const uint16_t kGreen = rgb(80, 210, 120);

struct Raster {
    uint16_t px[W * H]{};
    void fillRect(int x, int y, int w, int h, uint16_t c) {
        for (int yy = std::max(0, y); yy < std::min(H, y + h); ++yy)
            for (int xx = std::max(0, x); xx < std::min(W, x + w); ++xx) px[yy * W + xx] = c;
    }
    void drawPixel(int x, int y, uint16_t c) { if (x >= 0 && x < W && y >= 0 && y < H) px[y * W + x] = c; }
    uint16_t readPixel(int x, int y) const { return (x >= 0 && x < W && y >= 0 && y < H) ? px[y * W + x] : 0; }
    void blendPixel(int x, int y, uint16_t c, int a) {
        if (x < 0 || x >= W || y < 0 || y >= H || a <= 0) return;
        px[y * W + x] = a >= 255 ? c : mix(c, px[y * W + x], a);
    }
    // Darken a region (a scrim) — cheap on device: one pass over the sprite.
    void dim(int x, int y, int w, int h, int keep /*0..255*/) {
        for (int yy = std::max(0, y); yy < std::min(H, y + h); ++yy)
            for (int xx = std::max(0, x); xx < std::min(W, x + w); ++xx) px[yy * W + xx] = mix(px[yy * W + xx], kBlack, keep);
    }
    // Anti-aliased rounded rectangle (fill only), optionally with alpha.
    void roundRect(int x, int y, int w, int h, int r, uint16_t c, int alpha = 255, bool topOnly = false) {
        for (int yy = 0; yy < h; ++yy) for (int xx = 0; xx < w; ++xx) {
            float dx = 0, dy = 0;
            if (xx < r) dx = r - xx - 0.5f; else if (xx >= w - r) dx = xx - (w - r) + 0.5f;
            if (yy < r) dy = r - yy - 0.5f; else if (yy >= h - r && !topOnly) dy = yy - (h - r) + 0.5f;
            float d = std::sqrt(dx * dx + dy * dy) - (r - 0.5f);
            int cov = (dx == 0 || dy == 0) ? 255 : int(std::clamp(0.5f - d, 0.f, 1.f) * 255);
            blendPixel(x + xx, y + yy, c, cov * alpha / 255);
        }
    }
    void disc(float cx, float cy, float r, uint16_t c, int alpha = 255) {
        for (int y = int(cy - r - 1); y <= int(cy + r + 1); ++y) for (int x = int(cx - r - 1); x <= int(cx + r + 1); ++x) {
            float d = std::hypot(x + 0.5f - cx, y + 0.5f - cy) - r;
            blendPixel(x, y, c, int(std::clamp(0.5f - d, 0.f, 1.f) * alpha));
        }
    }
    void ring(float cx, float cy, float r, float t, uint16_t c) {
        for (int y = int(cy - r - 2); y <= int(cy + r + 2); ++y) for (int x = int(cx - r - 2); x <= int(cx + r + 2); ++x) {
            float d = std::fabs(std::hypot(x + 0.5f - cx, y + 0.5f - cy) - r) - t / 2;
            blendPixel(x, y, c, int(std::clamp(0.5f - d, 0.f, 1.f) * 255));
        }
    }
    // Anti-aliased filled triangle via edge functions with 4x4 supersampling.
    void triangle(float x0, float y0, float x1, float y1, float x2, float y2, uint16_t c) {
        auto edge = [](float ax, float ay, float bx, float by, float px, float py) { return (bx - ax) * (py - ay) - (by - ay) * (px - ax); };
        int minx = int(std::min({x0, x1, x2})) - 1, maxx = int(std::max({x0, x1, x2})) + 1;
        int miny = int(std::min({y0, y1, y2})) - 1, maxy = int(std::max({y0, y1, y2})) + 1;
        float area = edge(x0, y0, x1, y1, x2, y2);
        for (int y = miny; y <= maxy; ++y) for (int x = minx; x <= maxx; ++x) {
            int hits = 0;
            for (int sy = 0; sy < 4; ++sy) for (int sx = 0; sx < 4; ++sx) {
                float px = x + (sx + 0.5f) / 4, py = y + (sy + 0.5f) / 4;
                float a = edge(x1, y1, x2, y2, px, py), b = edge(x2, y2, x0, y0, px, py), d = edge(x0, y0, x1, y1, px, py);
                if (area > 0 ? (a >= 0 && b >= 0 && d >= 0) : (a <= 0 && b <= 0 && d <= 0)) ++hits;
            }
            blendPixel(x, y, c, hits * 255 / 16);
        }
    }
    // Thick anti-aliased line (capsule).
    void line(float x0, float y0, float x1, float y1, float t, uint16_t c) {
        int minx = int(std::min(x0, x1) - t), maxx = int(std::max(x0, x1) + t);
        int miny = int(std::min(y0, y1) - t), maxy = int(std::max(y0, y1) + t);
        float vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy;
        for (int y = miny; y <= maxy; ++y) for (int x = minx; x <= maxx; ++x) {
            float px = x + 0.5f - x0, py = y + 0.5f - y0;
            float s = len2 > 0 ? std::clamp((px * vx + py * vy) / len2, 0.f, 1.f) : 0;
            float d = std::hypot(px - s * vx, py - s * vy) - t / 2;
            blendPixel(x, y, c, int(std::clamp(0.5f - d, 0.f, 1.f) * 255));
        }
    }
};

// --- Text -------------------------------------------------------------------

// Instrument text blended against whatever is already on the canvas.
inline void text(Raster& c, const char* s, int x, int y, int px, uint16_t ink) {
    const auto& f = ui::proportional::font(px);
    const int baseline = y - ui::proportional::top(s, px);
    unsigned advance = 0;
    for (; *s; ++s) {
        const auto& g = ui::proportional::glyph(*s, px);
        const int left = x + int((advance + 32) / 64) + g.x;
        for (int row = 0; row < g.h; ++row) for (int col = 0; col < g.w; ++col)
            c.blendPixel(left + col, baseline + g.y + row, ink, f.pixels[g.offset + row * g.w + col]);
        advance += g.advance;
    }
}
inline int textW(const char* s, int px) { return ui::proportional::width(s, px); }
inline int capH(int px) { return ui::proportional::height("B", px); }
// Vertically centre cap height inside a box.
inline void textMid(Raster& c, const char* s, int x, int y, int h, int px, uint16_t ink) {
    text(c, s, x, y + (h - capH(px)) / 2, px, ink);
}
inline void textCenter(Raster& c, const char* s, int cx, int y, int px, uint16_t ink) { text(c, s, cx - textW(s, px) / 2, y, px, ink); }

// The GLCD 5x7 face used by scene labels today (TFT font 1).
inline void glcd(Raster& c, const char* s, int x, int y, int scale, uint16_t fg, uint16_t bg) {
    const int n = (int)strlen(s);
    c.fillRect(x, y, n * 6 * scale, 8 * scale, bg);
    for (int j = 0; j < n; ++j) for (int col = 0; col < 5; ++col) for (int row = 0; row < 8; ++row)
        if (font[(unsigned char)s[j] * 5 + col] & (1 << row)) c.fillRect(x + j * 6 * scale + col * scale, y + row * scale, scale, scale, fg);
}
inline void glcdCenter(Raster& c, const char* s, int x, int y, int w, int h, int scale, uint16_t fg, uint16_t bg) {
    const int tw = (int)strlen(s) * 6 * scale, th = 8 * scale;
    glcd(c, s, x + (w - tw) / 2, y + (h - th) / 2, scale, fg, bg);
}

// --- Sample media (procedural placeholders) ---------------------------------

enum class Sample { Sunset, Lake, Street };

inline void paintSample(Raster& c, Sample s, int x0, int y0, int w, int h) {
    for (int y = 0; y < h; ++y) for (int x = 0; x < w; ++x) {
        const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
        float r, g, b;
        if (s == Sample::Sunset) {
            r = 255 - 40 * v; g = 120 + 90 * (1 - v) * (1 - v); b = 90 + 110 * (1 - v);
            float sun = std::hypot((u - 0.68f) * 1.33f, v - 0.52f);
            if (sun < 0.11f) { r = 255; g = 236; b = 180; }
            else if (sun < 0.2f) { float k = (0.2f - sun) / 0.09f; r += (255 - r) * k * 0.5f; g += (220 - g) * k * 0.5f; b += (170 - b) * k * 0.4f; }
            float hill1 = 0.62f + 0.06f * std::sin(u * 7.0f + 1.0f) + 0.03f * std::sin(u * 17.f);
            float hill2 = 0.74f + 0.05f * std::sin(u * 5.0f + 3.0f);
            if (v > hill1) { r = 120; g = 62; b = 92; }
            if (v > hill2) { r = 58; g = 34; b = 62; }
            if (v > 0.9f) { r = 34; g = 22; b = 40; }
        } else if (s == Sample::Lake) {
            r = 150 + 70 * v; g = 196 + 40 * v; b = 236;
            float m = 0.48f - 0.22f * std::max(0.f, 1 - std::fabs(u - 0.35f) * 3.2f) - 0.14f * std::max(0.f, 1 - std::fabs(u - 0.72f) * 4.f);
            if (v > m) { r = 92; g = 108; b = 126; if (v < m + 0.05f) { r = 240; g = 244; b = 250; } }
            if (v > 0.56f) { r = 44; g = 92; b = 70; float t = 0.62f + 0.02f * std::sin(u * 40.f); if (v < t && std::fmod(u * 23.f, 1.f) < 0.5f) { r = 30; g = 70; b = 52; } }
            if (v > 0.64f) { float k = (v - 0.64f) / 0.36f; r = 60 + 30 * k; g = 128 + 20 * k; b = 168 - 30 * k; if (std::fmod(v * 60 + std::sin(u * 9) * 2, 3.f) < 0.5f) { r += 30; g += 30; b += 30; } }
        } else {
            r = 236; g = 210; b = 168;
            r -= 30 * v; g -= 40 * v; b -= 40 * v;
            auto bldg = [&](float a, float bb, float top, int cr, int cg, int cb) {
                if (u > a && u < bb && v > top) { r = cr; g = cg; b = cb; if (std::fmod(u * 70.f, 2.f) < 0.8f && std::fmod(v * 40.f, 2.f) < 0.9f && v < 0.8f) { r = 255; g = 214; b = 120; } }
            };
            bldg(0.02f, 0.22f, 0.22f, 70, 84, 110); bldg(0.2f, 0.38f, 0.38f, 96, 108, 132); bldg(0.6f, 0.82f, 0.18f, 64, 72, 96); bldg(0.8f, 0.99f, 0.33f, 88, 96, 120);
            if (v > 0.8f) { r = 60; g = 60; b = 66; if (std::fabs(v - 0.9f) < 0.008f && std::fmod(u * 10, 1.f) < 0.5f) { r = 230; g = 230; b = 220; } }
            float tree = std::hypot((u - 0.5f) * 1.3f, v - 0.5f);
            if (tree < 0.16f) { r = 62; g = 130; b = 80; }
            if (std::fabs(u - 0.5f) < 0.012f && v > 0.6f && v < 0.82f) { r = 92; g = 64; b = 44; }
        }
        if (x0 + x >= 0 && x0 + x < W && y0 + y >= 0 && y0 + y < H)  // clip (cover mode overhangs)
            c.px[(y0 + y) * W + (x0 + x)] = rgb(int(r), int(g), int(b));
    }
}
// Fit a 4:3 capture inside a box, centred (matches PhotoPreview::draw).
// Cover: scale a 4:3 capture so its height fills the panel, centred, cropping
// the left and right edges (280x240 shows the middle 280 of a 320-wide image).
inline void coverSample(Raster& c, Sample s) {
    const int dh = H, dw = H * 4 / 3;
    paintSample(c, s, (W - dw) / 2, 0, dw, dh);
}

inline void fitSample(Raster& c, Sample s, int x, int y, int w, int h, int* outY = nullptr, int* outH = nullptr) {
    int dw = w, dh = w * 3 / 4;
    if (dh > h) { dh = h; dw = h * 4 / 3; }
    const int ox = x + (w - dw) / 2, oy = y + (h - dh) / 2;
    paintSample(c, s, ox, oy, dw, dh);
    if (outY) *outY = oy;
    if (outH) *outH = dh;
}

// --- Icons (drawn, no assets) -----------------------------------------------

inline void iconPlay(Raster& c, float cx, float cy, float s, uint16_t ink) {
    c.triangle(cx - s * 0.38f, cy - s * 0.5f, cx - s * 0.38f, cy + s * 0.5f, cx + s * 0.5f, cy, ink);
}
inline void iconUpload(Raster& c, float cx, float cy, uint16_t ink) {
    c.line(cx, cy + 7, cx, cy - 6, 2.2f, ink);
    c.line(cx - 5.5f, cy - 1, cx, cy - 6.5f, 2.2f, ink);
    c.line(cx + 5.5f, cy - 1, cx, cy - 6.5f, 2.2f, ink);
    c.line(cx - 8, cy + 8, cx + 8, cy + 8, 2.0f, ink);
}
inline void iconTrash(Raster& c, float cx, float cy, uint16_t ink) {
    c.line(cx - 8, cy - 5, cx + 8, cy - 5, 2.0f, ink);
    c.line(cx - 3, cy - 8, cx + 3, cy - 8, 2.0f, ink);
    c.line(cx - 6, cy - 4, cx - 5, cy + 8, 2.0f, ink);
    c.line(cx + 6, cy - 4, cx + 5, cy + 8, 2.0f, ink);
    c.line(cx - 5, cy + 8, cx + 5, cy + 8, 2.0f, ink);
    c.line(cx - 1.5f, cy - 1, cx - 1.5f, cy + 5, 1.6f, ink);
    c.line(cx + 1.5f, cy - 1, cx + 1.5f, cy + 5, 1.6f, ink);
}
inline void iconCheck(Raster& c, float cx, float cy, float s, uint16_t ink, float t = 2.2f) {
    c.line(cx - s * 0.5f, cy, cx - s * 0.15f, cy + s * 0.35f, t, ink);
    c.line(cx - s * 0.15f, cy + s * 0.35f, cx + s * 0.5f, cy - s * 0.35f, t, ink);
}
inline void iconCloud(Raster& c, float cx, float cy, uint16_t ink, uint16_t paper) {
    c.disc(cx - 5, cy + 1, 4.5f, ink); c.disc(cx + 1, cy - 2, 6, ink); c.disc(cx + 6, cy + 2, 4, ink);
    c.roundRect(int(cx - 9), int(cy + 1), 19, 6, 3, ink);
    (void)paper;
}
inline void iconPicture(Raster& c, int x, int y, int w, int h, uint16_t ink) {
    c.roundRect(x, y, w, h, 6, ink);
    c.roundRect(x + 3, y + 3, w - 6, h - 6, 4, kBlack);
    c.disc(x + w * 0.68f, y + h * 0.34f, 4.5f, ink);
    c.triangle(x + 8, y + h - 8, x + w * 0.38f, y + h * 0.4f, x + w * 0.66f, y + h - 8, ink);
    c.triangle(x + w * 0.5f, y + h - 8, x + w * 0.7f, y + h * 0.58f, x + w - 8, y + h - 8, ink);
}

// --- Current staging layout (src/apps/photos_app.cpp) -----------------------

// Approximation of the system status strip: Wi-Fi left, clock centre, battery right.
inline void statusBar(Raster& c) {
    c.fillRect(0, 0, W, kStatusBarH, kWhite);
    for (int i = 0; i < 3; ++i) {  // redraw as a quarter fan pointing up
        for (int y = 8; y < 32; ++y) for (int x = 12; x < 48; ++x) {
            float d = std::hypot(x + 0.5f - 30, y + 0.5f - 30), a = std::atan2(30 - (y + 0.5f), x + 0.5f - 30);
            if (a > 0.75f && a < 2.4f && std::fabs(d - (7 + i * 7.f)) < 1.6f) c.px[y * W + x] = kBlack;
        }
    }
    c.disc(30, 29, 2.6f, kBlack);
    textCenter(c, "9:41", 140, 12, 18, kBlack);
    c.roundRect(226, 11, 36, 18, 4, kBlack); c.roundRect(228, 13, 32, 14, 3, kWhite);
    c.fillRect(230, 15, 22, 10, kBlack); c.fillRect(262, 16, 3, 8, kBlack);
}

inline void currentBrowse(Raster& c, Sample s, bool video, const char* name, int idx, int count) {
    c.fillRect(0, 0, W, H, kWhite);
    statusBar(c);
    fitSample(c, s, 0, kStatusBarH, W, H - kStatusBarH - 22);
    char indexBuf[24]; snprintf(indexBuf, sizeof indexBuf, "%d / %d", idx, count);
    char footer[64];
    int nameChars = (W - 16) / 6 - (int)strlen(indexBuf) - 2;
    snprintf(footer, sizeof footer, "%.*s  %s", nameChars, name, indexBuf);
    c.fillRect(0, H - 24, W, 24, kWhite);
    glcdCenter(c, footer, 14, H - 22, W - 28, 20, 1, kBlack, kWhite);
    if (video) {
        const int x = W / 2, y = H / 2;
        c.line(x - 8, y - 12, x + 12, y, 1, kWhite); c.line(x + 12, y, x - 8, y + 12, 1, kWhite); c.line(x - 8, y + 12, x - 8, y - 12, 1, kWhite);
        c.fillRect(0, H - 48, W, 24, kBlack);
        glcdCenter(c, "B: PLAY  0:12", 0, H - 48, W, 24, 1, kWhite, kBlack);
    }
}

inline void currentActions(Raster& c, Sample s, int selected) {
    currentBrowse(c, s, false, "IMG_0042.jpg", 3, 12);
    const int width = 122, height = 32, gap = 6, x = W - 14 - width, rows = 3;
    const int top = kStatusBarH + (H - kStatusBarH - 22 - height * rows - gap * (rows - 1)) / 2;
    const char* labels[] = {"Esc", "Upload", "Delete"};
    for (int row = 0; row < rows; ++row) {
        const bool sel = row == selected;
        const uint16_t bg = sel ? kBlack : kWhite, fg = sel ? kWhite : kBlack;
        const int y = top + row * (height + gap);
        c.fillRect(x, y, width, height, bg);
        c.fillRect(x, y, width, 1, fg); c.fillRect(x, y + height - 1, width, 1, fg);
        c.fillRect(x, y, 1, height, fg); c.fillRect(x + width - 1, y, 1, height, fg);
        if (sel) { c.line(x + 6, y + 9, x + 13, y + 16, 3, fg); c.line(x + 6, y + 23, x + 13, y + 16, 3, fg); }
        glcdCenter(c, labels[row], x + 18, y, 76, height, 2, fg, bg);
        const int ix = x + width - 23, iy = y + 8;
        if (row == 1) { c.line(ix + 8, iy + 16, ix + 8, iy, 2, fg); c.line(ix + 2, iy + 6, ix + 8, iy, 2, fg); c.line(ix + 14, iy + 6, ix + 8, iy, 2, fg); }
        else if (row == 2) iconTrash(c, ix + 8, iy + 9, fg);
        else { c.line(ix, iy + 8, ix + 16, iy + 8, 2, fg); c.line(ix + 10, iy + 2, ix + 16, iy + 8, 2, fg); c.line(ix + 10, iy + 14, ix + 16, iy + 8, 2, fg); }
    }
}

// --- Loading screens in today's firmware ----------------------------------
// Reproductions of the painters used during Photos/video transitions. Labels
// use TFT font 1 (GLCD 5x7) exactly as the device does; the status strip is
// the approximation above.

constexpr uint16_t kUiMuted = 0x4208, kUiAccent = 0x10A2;  // config/ui_theme.h

// TFT_eSPI MC_DATUM for font 1: centre the 6n x 8n cell run on (cx, cy).
inline void glcdMiddle(Raster& c, const char* s, int cx, int cy, int scale, uint16_t fg, uint16_t bg) {
    const int tw = (int)strlen(s) * 6 * scale;
    glcd(c, s, cx - tw / 2, cy - 4 * scale, scale, fg, bg);
}

// ui::paintAppOpeningSplash (src/ui/draw.cpp) + status bar, shown while the
// gallery catalog loads on entry.
inline void currentOpeningSplash(Raster& c, const char* app) {
    c.fillRect(0, 0, W, H, kWhite);
    statusBar(c);
    const int midY = kStatusBarH + (H - kStatusBarH) / 2;
    glcdMiddle(c, "Opening", W / 2, midY - 36, 1, kUiMuted, kWhite);
    glcdMiddle(c, app, W / 2, midY - 6, 3, kBlack, kWhite);
    glcdMiddle(c, "...", W / 2, midY + 28, 1, kUiAccent, kWhite);
}

// Videos app before its first frame (src/apps/videos_app.cpp drawScene):
// panelHeader("Videos") + labelCenterMuted(message), status bar visible.
inline void currentVideosLoading(Raster& c, const char* message) {
    c.fillRect(0, 0, W, H, kWhite);
    statusBar(c);
    glcd(c, "Videos", 14, kStatusBarH + 6, 2, kBlack, kWhite);
    const int y = kStatusBarH + 30 + 3;
    glcdMiddle(c, message, W / 2, y + 4, 1, kUiMuted, kWhite);
}

// --- Proposed layout: the firmware's own view (include/ui/photos_view.h) ---

namespace pv = ui::photos_view;
inline pv::Bounds screen() { return {W, H}; }

struct Media { Sample sample; bool video; const char* durationText; bool uploaded; };

// Mirrors onDraw() in src/apps/photos_app.cpp: black, fitted media, chrome.
inline pv::ImageRows fullBleed(Raster& c, const Media& m) {
    c.fillRect(0, 0, W, H, kBlack);
    fitSample(c, m.sample, 0, 0, W, H);
    return pv::findImageRows(c, screen());
}

inline void proposedBrowse(Raster& c, const Media& m, bool scrolling, int idx, int count) {
    const auto rows = fullBleed(c, m);
    if (m.video) {
        pv::playBadge(c, screen(), W / 2, (rows.top + rows.bottom) / 2);
        pv::durationChip(c, screen(), m.durationText, W - 8, rows.bottom - 8);
    }
    if (scrolling) pv::positionReadout(c, screen(), idx, count, rows.bottom);
}

inline void proposedSheet(Raster& c, const Media& m, const pv::SheetRow* rows, int n, int selected, const char* kind,
                          int idx, int count, const char* error = nullptr) {
    fullBleed(c, m);
    pv::sheet(c, screen(), pv::SheetModel{kind, idx, count, m.uploaded, error, rows, n, selected}, true);
}

inline void proposedConfirm(Raster& c, const Media& m, bool deleteFocused) {
    fullBleed(c, m);
    pv::confirmDelete(c, screen(), m.video, deleteFocused, true);
}

inline void proposedNotice(Raster& c, const Media& m, const char* label) {
    const auto rows = fullBleed(c, m);
    if (m.video) pv::playBadge(c, screen(), W / 2, (rows.top + rows.bottom) / 2);  // chip yields to the notice
    pv::notice(c, screen(), label, true);
}

// Photos on a video item before its thumbnail decodes (also right after
// returning from the player): black panel + "Loading preview..." (photos_app.cpp).
inline void currentThumbnailLoading(Raster& c) {
    c.fillRect(0, 0, W, H, kBlack);
    pv::textCenter(c, screen(), "Loading preview...", W / 2, H / 2 - pv::capHeight(18) / 2, 18, pv::kMuted);
}

// Photos while the library handoff is pending: s_error = "Preparing preview..."
// rendered through the Error message, including its retry line.
inline void currentPreparing(Raster& c) {
    pv::message(c, screen(), "Preparing preview...", "Press the dial to try again.", nullptr);
}

// Cover-mode browse: no letterbox, so the position readout uses its chip.
inline void proposedBrowseCover(Raster& c, const Media& m, bool scrolling, int idx, int count) {
    c.fillRect(0, 0, W, H, kBlack);
    coverSample(c, m.sample);
    if (m.video) {
        pv::playBadge(c, screen(), W / 2, H / 2);
        pv::durationChip(c, screen(), m.durationText, W - 8, H - 8);
    }
    if (scrolling) pv::positionReadout(c, screen(), idx, count, H);
}

// Full-screen reference layout: cover-cropped media, index pill on top, and
// capture date/time at the bottom (omitted when the capture time is unknown).
inline void proposedFullscreen(Raster& c, const Media& m, int idx, int count,
                               const char* date, const char* time) {
    c.fillRect(0, 0, W, H, kBlack);
    coverSample(c, m.sample);
    if (m.video) pv::playBadge(c, screen(), W / 2, H / 2);
    pv::indexPill(c, screen(), idx, count);
    const char* parts[] = {date, time, m.video ? m.durationText : nullptr};
    pv::dateCaption(c, screen(), parts, 3);
}

// Video playback with the title card (as CameraPreview::presentVideo).
inline void proposedVideoCard(Raster& c, Sample sample, const pv::VideoCard& card, int alpha) {
    c.fillRect(0, 0, W, H, kBlack);
    coverSample(c, sample);
    pv::videoCard(c, screen(), card, alpha);
}

inline void proposedEmpty(Raster& c) {
    pv::message(c, screen(), "No photos yet", "Photos and videos from Camera", "will show up here.");
}

}  // namespace photos_study
