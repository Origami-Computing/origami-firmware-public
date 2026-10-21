#pragma once
/**
 * Tennis: fast drawing straight into the kami's canvas buffer (the 280x240 TFT_eSprite: row-major RGB565, each
 * pixel stored byte-swapped). doodle_draw::text()/sprite() go through TFT_eSprite::readPixel/drawPixel per pixel,
 * which cost most of a tennis frame; these write the buffer directly and give the same pixels.
 *
 * Colours in and out are plain RGB565; only the buffer is byte-swapped. Everything is clipped to the canvas's clip
 * rectangle (below the status bar for a whole frame, or one dirty rectangle), and a pixel's value never depends on
 * the clip, so repainting a rectangle gives the same pixels as repainting the frame.
 * Pure (no Arduino): host-tested in test/test_tennis_raster/test_host.cpp.
 */
#include <stdint.h>

#include "assets/doodle_notebook.h"

namespace ui {
namespace tennis {
namespace raster {

struct Canvas {
  uint16_t* px;         // row-major, w wide, h high, byte-swapped RGB565
  int w, h;
  int x0, y0, x1, y1;   // clip, half-open; y0 >= 38 keeps off the status bar
};

inline uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }

// floor(n / 255) without a division, exact for 0 <= n < 65535 (the blend needs at most 63 * 255 + 127).
inline uint32_t div255(uint32_t n) { return (n + 1 + (n >> 8)) >> 8; }

// Same result as doodle_draw::blend(): per channel (fg * a + bg * (255 - a) + 127) / 255.
inline uint16_t blend(uint16_t fg, uint16_t bg, unsigned a) {
  const unsigned inv = 255 - a;
  const uint32_t r = div255((fg >> 11) * a + (bg >> 11) * inv + 127);
  const uint32_t g = div255(((fg >> 5) & 63) * a + ((bg >> 5) & 63) * inv + 127);
  const uint32_t b = div255((fg & 31) * a + (bg & 31) * inv + 127);
  return (uint16_t)((r << 11) | (g << 5) | b);
}

// Blend one pixel into a (byte-swapped) buffer slot.
inline void blendInto(uint16_t& d, uint16_t col, uint8_t a) {
  if (a == 255) { d = swap16(col); return; }
  d = swap16(blend(col, swap16(d), a));
}

inline void blendPx(const Canvas& c, int x, int y, uint16_t col, uint8_t a) {
  if (!a || x < c.x0 || x >= c.x1 || y < c.y0 || y >= c.y1) return;
  blendInto(c.px[y * c.w + x], col, a);
}

inline int textWidth(const char* text, const doodle_art::Font& font) {
  int width = 0;
  for (; *text; ++text) {
    const unsigned ch = (unsigned char)*text;
    if (ch >= 32 && ch < 127) width += font.glyphs[ch - 32].advance;
  }
  return width;
}

// doodle_draw::text() semantics: y is the alphabetic baseline; align 0 = left, 1 = centre, 2 = right.
inline void text(const Canvas& c, const char* value, int x, int baseline, const doodle_art::Font& font, uint16_t color,
                 int align = 0) {
  const int width = textWidth(value, font);
  if (align == 1) x -= width / 2;
  else if (align == 2) x -= width;
  const uint16_t solid = swap16(color);
  for (; *value; ++value) {
    const unsigned ch = (unsigned char)*value;
    if (ch < 32 || ch >= 127) continue;
    const doodle_art::Glyph& g = font.glyphs[ch - 32];
    const int gx = x + g.x, gy = baseline + g.y;
    const int x0 = gx < c.x0 ? c.x0 - gx : 0, x1 = gx + g.w > c.x1 ? c.x1 - gx : g.w;
    const int y0 = gy < c.y0 ? c.y0 - gy : 0, y1 = gy + g.h > c.y1 ? c.y1 - gy : g.h;
    if (x0 >= x1 || y0 >= y1) { x += g.advance; continue; }
    for (int row = y0; row < y1; ++row) {
      const uint8_t* a = font.alpha + g.offset + row * g.w;
      uint16_t* d = c.px + (gy + row) * c.w + gx;
      for (int col = x0; col < x1; ++col) {
        const uint8_t al = a[col];
        if (!al) continue;
        if (al == 255) d[col] = solid;
        else d[col] = swap16(blend(color, swap16(d[col]), al));
      }
    }
    x += g.advance;
  }
}

// A sprite with per-pixel alpha, top-left at (x, y).
inline void blit(const Canvas& c, const doodle_art::Sprite& img, int x, int y) {
  const int x0 = x < c.x0 ? c.x0 - x : 0, x1 = x + img.w > c.x1 ? c.x1 - x : img.w;
  const int y0 = y < c.y0 ? c.y0 - y : 0, y1 = y + img.h > c.y1 ? c.y1 - y : img.h;
  for (int row = y0; row < y1; ++row) {
    const uint16_t* p = img.pixels + row * img.w;
    const uint8_t* a = img.alpha + row * img.w;
    uint16_t* d = c.px + (y + row) * c.w + x;
    for (int col = x0; col < x1; ++col)
      if (a[col]) blendInto(d[col], p[col], a[col]);
  }
}

// An opaque rectangle.
inline void fillRect(const Canvas& c, int x, int y, int w, int h, uint16_t color) {
  const int xa = x < c.x0 ? c.x0 : x, xb = x + w > c.x1 ? c.x1 : x + w;
  const int ya = y < c.y0 ? c.y0 : y, yb = y + h > c.y1 ? c.y1 : y + h;
  const uint16_t v = swap16(color);
  for (int yy = ya; yy < yb; ++yy)
    for (int xx = xa; xx < xb; ++xx) c.px[yy * c.w + xx] = v;
}

// A translucent panel with rounded corners of radius r (the tennis HUD's boxes, the how-to-play card).
inline void panel(const Canvas& c, int x, int y, int w, int h, uint16_t color, uint8_t alpha, int r = 4) {
  if (!alpha) return;
  const unsigned inv = 255 - alpha;
  const uint32_t fr = (color >> 11) * alpha + 127, fg = ((color >> 5) & 63) * alpha + 127, fb = (color & 31) * alpha + 127;
  for (int yy = y < c.y0 ? c.y0 : y; yy < y + h && yy < c.y1; ++yy) {
    const int ey = yy < y + r ? y + r - yy : (yy >= y + h - r ? yy - (y + h - r - 1) : 0);
    uint16_t* d = c.px + yy * c.w;
    for (int xx = x < c.x0 ? c.x0 : x; xx < x + w && xx < c.x1; ++xx) {
      const int ex = xx < x + r ? x + r - xx : (xx >= x + w - r ? xx - (x + w - r - 1) : 0);
      if (ex * ex + ey * ey > r * r + 1) continue;  // rounded corners
      const uint16_t bg = swap16(d[xx]);
      const uint32_t r = div255(fr + (bg >> 11) * inv), g = div255(fg + ((bg >> 5) & 63) * inv),
                     b = div255(fb + (bg & 31) * inv);
      d[xx] = swap16((uint16_t)((r << 11) | (g << 5) | b));
    }
  }
}

}  // namespace raster
}  // namespace tennis
}  // namespace ui
