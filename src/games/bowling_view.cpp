// Bowling drawing: see include/games/bowling_view.h.
#include "games/bowling_view.h"

#include <initializer_list>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "assets/bowling_art.h"
#include "games/bowling_help.h"

namespace games {
namespace bowling_view {

namespace A = bowling_art;
namespace B = core::bowling;

namespace {

constexpr int kW = 280, kH = 240, kTop = 38;
constexpr float kPi = 3.14159265f;
uint16_t* g_px = nullptr;

constexpr uint16_t rgb(unsigned r, unsigned g, unsigned b) {
  return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}
inline uint16_t swap16(uint16_t c) { return (uint16_t)((c >> 8) | (c << 8)); }
inline unsigned div255(unsigned v) { v += 128; return (v + (v >> 8)) >> 8; }   // exact v/255 for v < 65536
inline uint16_t mix(uint16_t fg, uint16_t bg, unsigned a) {
  const unsigned inv = 255 - a;
  const unsigned r = div255((fg >> 11) * a + (bg >> 11) * inv);
  const unsigned g = div255(((fg >> 5) & 63) * a + ((bg >> 5) & 63) * inv);
  const unsigned b = div255((fg & 31) * a + (bg & 31) * inv);
  return (uint16_t)((r << 11) | (g << 5) | b);
}
inline uint16_t shade(uint16_t c, float k) {           // k < 1 darkens, > 1 brightens
  int r = (int)((c >> 11) * k), g = (int)(((c >> 5) & 63) * k), b = (int)((c & 31) * k);
  r = r > 31 ? 31 : r; g = g > 63 ? 63 : g; b = b > 31 ? 31 : b;
  return (uint16_t)((r << 11) | (g << 5) | b);
}
// ---- retained drawing ----
// Every frame runs the drawing code twice. Measure: each primitive only reports its footprint and a hash of its
// parameters to the current item (the ball, the bowler, the scorecard, ...). Items whose hash or box changed since
// the last frame mark their old and new boxes dirty. Paint: the background is restored in the dirty rectangles only,
// and the drawing code runs once per rectangle, clipped to it; primitives outside it return at once. Static parts
// (the scorecard, a resting bowler, standing pins) cost nothing, and the canvas (PSRAM) is barely touched.
enum class Mode : uint8_t { Measure, Paint };
Mode g_mode = Mode::Paint;
int g_item = 0;
int g_cx0 = 0, g_cy0 = kTop, g_cx1 = kW, g_cy1 = kH;         // paint clip, half-open
Retained* g_ret = nullptr;                                    // where the measure pass writes (PSRAM)

enum Item : int {
  kIPins, kIAim, kILaneBall, kIBowler, kIDeckPin0, kIDeckBall = kIDeckPin0 + 10, kIScore, kIDiagram, kIHint,
  kISpeed, kIBanner, kISpark0, kIOver = kISpark0 + 6, kICenter, kIHelp, kIHelpText, kItemCount
};
static_assert(kItemCount <= Retained::kItems, "items");
inline void item(int id) { g_item = id; }

using games::dirty::Hash;

// A primitive's footprint [x0,x1) x [y0,y1). Measure: record it, return false. Paint: clip it, true if non-empty.
bool claim(int& x0, int& y0, int& x1, int& y1, uint32_t h) {
  if (x0 < 0) x0 = 0;
  if (y0 < kTop) y0 = kTop;
  if (x1 > kW) x1 = kW;
  if (y1 > kH) y1 = kH;
  if (g_mode == Mode::Measure) {
    Rect& a = g_ret->cur[g_item];
    uint32_t& sig = g_ret->curSig[g_item];
    sig = (sig ^ h) * 16777619u + 0x9e3779b9u;
    if (x0 < x1 && y0 < y1) {
      if (x0 < a.x0) a.x0 = (int16_t)x0;
      if (y0 < a.y0) a.y0 = (int16_t)y0;
      if (x1 > a.x1) a.x1 = (int16_t)x1;
      if (y1 > a.y1) a.y1 = (int16_t)y1;
    }
    return false;
  }
  if (x0 < g_cx0) x0 = g_cx0;
  if (y0 < g_cy0) y0 = g_cy0;
  if (x1 > g_cx1) x1 = g_cx1;
  if (y1 > g_cy1) y1 = g_cy1;
  return x0 < x1 && y0 < y1;
}

uint32_t g_blends = 0;                                        // pixels written this frame (the device cost)
inline void blendAt(uint16_t& d, uint16_t col, unsigned a) {
  ++g_blends;
  if (a >= 255) { d = swap16(col); return; }
  d = swap16(mix(col, swap16(d), a));
}
// Single pixel inside an already claimed primitive: clipped, never in the measure pass.
inline void px(int x, int y, uint16_t col, unsigned a) {
  if (!a || g_mode == Mode::Measure || x < g_cx0 || x >= g_cx1 || y < g_cy0 || y >= g_cy1) return;
  blendAt(g_px[y * kW + x], col, a);
}
inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

// sin for the ball's marbling: a parabola fit, ~0.1% error, no libm call per pixel
inline float fastSin(float x) {
  x *= 0.15915494f;                                          // turns
  x -= floorf(x + 0.5f);                                     // -0.5..0.5
  const float y = 8.0f * x - 16.0f * x * fabsf(x);
  return 0.225f * (y * fabsf(y) - y) + y;
}

// ---- cameras ----
struct Cam { float y, h, f, cx, hy; };
const Cam kCamA = {A::kACamY, A::kACamH, A::kAF, A::kACx, A::kAHy};
const Cam kCamD = {A::kDCamY, A::kDCamH, A::kDF, A::kDCx, A::kDHy};
inline bool proj(const Cam& c, float x, float y, float z, float* sx, float* sy) {
  const float d = y - c.y;
  if (d < 0.05f) return false;
  *sx = c.cx + x * c.f / d;
  *sy = c.hy + (c.h - z) * c.f / d;
  return true;
}

// ---- primitives ----
void fillRect(int x, int y, int w, int h, uint16_t col, unsigned a) {
  int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
  if (!claim(x0, y0, x1, y1, Hash(1).u(x).u(y).u(w).u(h).u(col).u(a).v)) return;
  for (int yy = y0; yy < y1; ++yy) {
    uint16_t* d = g_px + yy * kW;
    for (int xx = x0; xx < x1; ++xx) blendAt(d[xx], col, a);
  }
}

void panel(int x, int y, int w, int h, uint16_t col, unsigned a, int r = 4) {
  int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
  if (!claim(x0, y0, x1, y1, Hash(2).u(x).u(y).u(w).u(h).u(col).u(a).u(r).v)) return;
  const unsigned half = a / 2;
  for (int yy = y0; yy < y1; ++yy) {
    const int ey = yy < y + r ? y + r - yy : (yy >= y + h - r ? yy - (y + h - r - 1) : 0);
    uint16_t* d = g_px + yy * kW;
    for (int xx = x0; xx < x1; ++xx) {
      const int ex = xx < x + r ? x + r - xx : (xx >= x + w - r ? xx - (x + w - r - 1) : 0);
      if (!(ex | ey)) { blendAt(d[xx], col, a); continue; }
      const int d2 = ex * ex + ey * ey;
      if (d2 > r * r) continue;
      blendAt(d[xx], col, d2 > (r - 1) * (r - 1) ? half : a);
    }
  }
}

void blit(const doodle_art::Sprite& s, int bx, int by) {
  int x0 = bx, y0 = by, x1 = bx + s.w, y1 = by + s.h;
  if (!claim(x0, y0, x1, y1, Hash(3).p(&s).u(bx).u(by).v)) return;
  for (int y = y0; y < y1; ++y) {
    const uint8_t* al = s.alpha + (y - by) * s.w;
    const uint16_t* pc = s.pixels + (y - by) * s.w;
    uint16_t* d = g_px + y * kW;
    for (int x = x0; x < x1; ++x)
      if (al[x - bx]) blendAt(d[x], pc[x - bx], al[x - bx]);
  }
}

// Scaled blit (nearest), with the sprite pixel (ax, ay) landing on screen (sx, sy). flipY mirrors it about that row
// (a reflection in the lane), fading with distance.
void blitScaled(const doodle_art::Sprite& s, int ax, int ay, float sx, float sy, float scale, bool mirror, bool flipY,
                unsigned alphaMul) {
  if (scale <= 0.05f) return;
  const float inv = 1.0f / scale;
  const float left = mirror ? sx - (s.w - 1 - ax + 0.5f) * scale : sx - (ax + 0.5f) * scale;
  const float top = flipY ? sy - (s.h - 1 - ay) * scale : sy - (ay + 0.5f) * scale;
  int x0 = (int)floorf(left), x1 = (int)ceilf(left + s.w * scale);
  int y0 = (int)floorf(top), y1 = (int)ceilf(top + s.h * scale);
  const Hash h = Hash(4).p(&s).f(sx).f(sy).f(scale).u(mirror).u(flipY).u(alphaMul);
  if (!claim(x0, y0, x1, y1, h.v)) return;
  const float span = s.h * scale;
  // columns: fixed point (16.16) source position, stepped per pixel
  // (the same for any clip, so a clipped repaint matches a full one)
  const int32_t step = (int32_t)(inv * 65536.0f);
  const int32_t base = (int32_t)((0.5f - left) * inv * 65536.0f);
  for (int y = y0; y < y1; ++y) {
    const int r = flipY ? (int)floorf(ay - (y + 0.5f - sy) * inv) : (int)floorf((y + 0.5f - top) * inv);
    if (r < 0 || r >= s.h) continue;
    unsigned am = alphaMul;
    if (flipY) {
      const float t = (y + 0.5f - sy) / span;                 // 0 at the base .. 1 at the far end
      am = (unsigned)(alphaMul * clampf(1.0f - t * 1.6f, 0, 1));
      if (!am) continue;
    }
    const uint8_t* al = s.alpha + r * s.w;
    const uint16_t* pc = s.pixels + r * s.w;
    uint16_t* d = g_px + y * kW;
    int32_t cf = base + x0 * step;
    for (int x = x0; x < x1; ++x, cf += step) {
      int c = cf >> 16;
      if (cf < 0 || c >= s.w) continue;
      if (mirror) c = s.w - 1 - c;
      const uint8_t a = al[c];
      if (a) blendAt(d[x], pc[c], am >= 255 ? a : div255(a * am));
    }
  }
}

void softEllipse(float cx, float cy, float rx, float ry, uint16_t col, unsigned alpha) {
  if (rx < 0.3f || ry < 0.3f) return;
  int x0 = (int)floorf(cx - rx - 1), x1 = (int)ceilf(cx + rx + 1) + 1;
  int y0 = (int)floorf(cy - ry - 1), y1 = (int)ceilf(cy + ry + 1) + 1;
  if (!claim(x0, y0, x1, y1, Hash(5).f(cx).f(cy).f(rx).f(ry).u(col).u(alpha).v)) return;
  const float irx = 1.0f / rx, iry = 1.0f / ry, m = fminf(rx, ry);
  for (int y = y0; y < y1; ++y) {
    const float dy = (y + 0.5f - cy) * iry;
    uint16_t* d = g_px + y * kW;
    for (int x = x0; x < x1; ++x) {
      const float dx = (x + 0.5f - cx) * irx;
      const float q = dx * dx + dy * dy;
      if (q >= (1.0f + 0.5f / m) * (1.0f + 0.5f / m)) continue;
      const float cover = clampf((1.0f - sqrtf(q)) * m + 0.5f, 0, 1);
      if (cover > 0) blendAt(d[x], col, (unsigned)(cover * alpha));
    }
  }
}

// A soft, feathered shadow: dark in the middle, fading out.
void shadow(float cx, float cy, float rx, float ry, unsigned alpha) {
  if (rx < 0.5f) return;
  int x0 = (int)floorf(cx - rx), x1 = (int)ceilf(cx + rx) + 1;
  int y0 = (int)floorf(cy - ry), y1 = (int)ceilf(cy + ry) + 1;
  if (!claim(x0, y0, x1, y1, Hash(6).f(cx).f(cy).f(rx).f(ry).u(alpha).v)) return;
  const float irx = 1.0f / rx, iry = 1.0f / ry;
  for (int y = y0; y < y1; ++y) {
    const float dy = (y + 0.5f - cy) * iry;
    uint16_t* d = g_px + y * kW;
    for (int x = x0; x < x1; ++x) {
      const float dx = (x + 0.5f - cx) * irx;
      const float d2 = dx * dx + dy * dy;
      if (d2 < 1) {
        const unsigned a = (unsigned)(alpha * (1 - d2) * (1 - d2));
        if (a) blendAt(d[x], rgb(20, 12, 8), a);
      }
    }
  }
}

// A limb: a capsule from (x0,y0) to (x1,y1), `sleeve` of its length in c0 then c1, lit from the left, outlined.
void limb(float x0, float y0, float x1, float y1, float r, uint16_t c0, uint16_t c1, float sleeve) {
  const float vx = x1 - x0, vy = y1 - y0, len2 = vx * vx + vy * vy + 1e-6f, len = sqrtf(len2);
  const float ro = r + 1.1f;
  int bx0 = (int)floorf(fminf(x0, x1) - ro - 1), bx1 = (int)ceilf(fmaxf(x0, x1) + ro + 1) + 1;
  int by0 = (int)floorf(fminf(y0, y1) - ro - 1), by1 = (int)ceilf(fmaxf(y0, y1) + ro + 1) + 1;
  if (!claim(bx0, by0, bx1, by1, Hash(7).f(x0).f(y0).f(x1).f(y1).f(r).u(c0).u(c1).f(sleeve).v)) return;
  const float ilen2 = 1.0f / len2, iside = 1.0f / (len * r + 1e-6f);
  const uint16_t edge = rgb(40, 30, 30);
  for (int y = by0; y < by1; ++y) {
    uint16_t* d = g_px + y * kW;
    for (int x = bx0; x < bx1; ++x) {
      const float qx = x + 0.5f - x0, qy = y + 0.5f - y0;
      const float t = clampf((qx * vx + qy * vy) * ilen2, 0, 1);
      const float ex = qx - t * vx, ey = qy - t * vy;
      const float e2 = ex * ex + ey * ey;
      if (e2 >= (ro + 0.5f) * (ro + 0.5f)) continue;
      const float dd = sqrtf(e2);
      const float outer = clampf(ro - dd + 0.5f, 0, 1);
      if (outer <= 0) continue;
      const float inner = clampf(r - dd + 0.5f, 0, 1);
      if (inner <= 0) { blendAt(d[x], edge, (unsigned)(255 * outer * 0.6f)); continue; }
      const float side = (ex * (-vy) + ey * vx) * iside;      // -1..1 across the limb
      const uint16_t base = t < sleeve ? c0 : c1;
      const uint16_t lit = shade(base, 1.0f + 0.18f * side - 0.25f * side * side);
      blendAt(d[x], mix(lit, edge, (unsigned)(255 * inner)), 255);
    }
  }
}

// ---- the ball: a lit sphere with marbling and finger holes that roll ----
const float kHoles[3][3] = {{0, -0.40f, 0.917f}, {-0.16f, 0.30f, 0.94f}, {0.16f, 0.30f, 0.94f}};

void drawBall(float cx, float cy, float r, float roll, unsigned alpha, bool detailed = true) {
  if (r < 0.6f) return;
  int x0 = (int)floorf(cx - r - 1), x1 = (int)ceilf(cx + r + 1) + 1;
  int y0 = (int)floorf(cy - r - 1), y1 = (int)ceilf(cy + r + 1) + 1;
  if (!claim(x0, y0, x1, y1, Hash(8).f(cx).f(cy).f(r).f(roll).u(alpha).u(detailed).v)) return;
  const float cr = cosf(roll), sr = sinf(roll);
  // light from the upper left, towards the viewer; half vector for the gloss
  const float lx = -0.45f, ly = -0.62f, lz = 0.64f;
  float hx = lx, hy = ly, hz = lz + 1;
  const float hn = sqrtf(hx * hx + hy * hy + hz * hz);
  hx /= hn; hy /= hn; hz /= hn;
  const bool detail = detailed && r >= 4.0f;
  const float ir = 1.0f / r, lim = 1.0f + 2.0f / r;
  for (int y = y0; y < y1; ++y) {
    const float ny = (y + 0.5f - cy) * ir;
    uint16_t* d = g_px + y * kW;
    for (int x = x0; x < x1; ++x) {
      const float nx = (x + 0.5f - cx) * ir;
      const float d2 = nx * nx + ny * ny;
      if (d2 >= lim) continue;
      const float len = sqrtf(d2);
      const float cover = clampf((1.0f - len) * r + 0.5f, 0, 1);
      if (cover <= 0) continue;
      const float nz = sqrtf(fmaxf(0.0f, 1.0f - d2));
      float cr_ = 30, cg = 70, cb = 190;                       // royal blue
      if (detail) {
        // the ball's own frame: rolling away from the camera turns it about the screen's x axis
        const float bx = nx, by = ny * cr - nz * sr, bz = ny * sr + nz * cr;
        const float m = fastSin(bx * 4.1f + by * 1.3f + fastSin(by * 3.3f + bz * 2.1f) * 2.4f);
        const float k = 0.5f + 0.5f * m;
        cr_ = 22 + 90 * k * k;                                  // swirls of violet in the blue
        cg = 52 + 50 * k;
        cb = 150 + 95 * k;
        for (int h = 0; h < 3; ++h) {
          const float c = bx * kHoles[h][0] + by * kHoles[h][1] + bz * kHoles[h][2];
          if (c > 0.982f) { cr_ = 12; cg = 12; cb = 22; }
          else if (c > 0.972f) { cr_ *= 0.55f; cg *= 0.55f; cb *= 0.6f; }
        }
      }
      const float lam = fmaxf(0.0f, nx * lx + ny * ly + nz * lz);
      float sp = fmaxf(0.0f, nx * hx + ny * hy + nz * hz);
      sp *= sp; sp *= sp; sp *= sp; sp *= sp; sp *= sp;       // ^32: a tight highlight
      const float rim = (1 - nz) * (1 - nz) * fmaxf(0.0f, ny) * 0.8f;   // the bright lane below
      const float amb = 0.30f + 0.85f * lam;
      const float R = cr_ * amb + 255 * sp + 150 * rim, G = cg * amb + 250 * sp + 120 * rim, Bc = cb * amb + 240 * sp + 90 * rim;
      const uint16_t col = rgb((unsigned)clampf(R, 0, 255), (unsigned)clampf(G, 0, 255), (unsigned)clampf(Bc, 0, 255));
      blendAt(d[x], col, (unsigned)(cover * alpha));
    }
  }
}

// ---- text (the doodle fonts, blended straight into the buffer) ----
int textW(const char* t, const doodle_art::Font& f) {
  int w = 0;
  for (; *t; ++t) {
    const unsigned ch = (unsigned char)*t;
    if (ch >= 32 && ch < 127) w += f.glyphs[ch - 32].advance;
  }
  return w;
}

void text(const char* t, int x, int base, const doodle_art::Font& f, uint16_t col, int align = 0, unsigned alpha = 255) {
  const int w = textW(t, f);
  if (align == 1) x -= w / 2;
  else if (align == 2) x -= w;
  // footprint: every glyph box
  int bx0 = kW, by0 = kH, bx1 = 0, by1 = 0, xx = x;
  for (const char* q = t; *q; ++q) {
    const unsigned ch = (unsigned char)*q;
    if (ch < 32 || ch >= 127) continue;
    const doodle_art::Glyph& g = f.glyphs[ch - 32];
    if (g.w && g.h) {
      if (xx + g.x < bx0) bx0 = xx + g.x;
      if (xx + g.x + g.w > bx1) bx1 = xx + g.x + g.w;
      if (base + g.y < by0) by0 = base + g.y;
      if (base + g.y + g.h > by1) by1 = base + g.y + g.h;
    }
    xx += g.advance;
  }
  if (!claim(bx0, by0, bx1, by1, Hash(9).s(t).u(x).u(base).p(&f).u(col).u(alpha).v)) return;
  for (; *t; ++t) {
    const unsigned ch = (unsigned char)*t;
    if (ch < 32 || ch >= 127) continue;
    const doodle_art::Glyph& g = f.glyphs[ch - 32];
    const int gx = x + g.x, gy = base + g.y;
    const int cx0 = gx < bx0 ? bx0 : gx, cx1 = gx + g.w > bx1 ? bx1 : gx + g.w;
    const int cy0 = gy < by0 ? by0 : gy, cy1 = gy + g.h > by1 ? by1 : gy + g.h;
    for (int yy = cy0; yy < cy1; ++yy) {
      const uint8_t* a = f.alpha + g.offset + (yy - gy) * g.w;
      uint16_t* d = g_px + yy * kW;
      for (int x2 = cx0; x2 < cx1; ++x2) {
        const uint8_t v = a[x2 - gx];
        if (v) blendAt(d[x2], col, alpha >= 255 ? v : div255(v * alpha));
      }
    }
    x += g.advance;
  }
}

// Outlined text for banners, in one pass: the glyphs are rasterised into a scratch alpha map (Retained, PSRAM), then
// each pixel composes edge (the outline ring, max of 8 neighbours), shade (one row down) and fill, and is written
// once. (Drawing it as 10 text passes wrote every pixel 10 times.)
uint8_t* g_scratch = nullptr;
void bigText(const char* t, int x, int base, const doodle_art::Font& f, uint16_t col, uint16_t edge) {
  const int w = textW(t, f);
  x -= w / 2;
  int gx0 = kW * 2, gy0 = kH * 2, gx1 = -kW, gy1 = -kH, xx = x;
  for (const char* q = t; *q; ++q) {
    const unsigned ch = (unsigned char)*q;
    if (ch < 32 || ch >= 127) continue;
    const doodle_art::Glyph& g = f.glyphs[ch - 32];
    if (g.w && g.h) {
      if (xx + g.x < gx0) gx0 = xx + g.x;
      if (xx + g.x + g.w > gx1) gx1 = xx + g.x + g.w;
      if (base + g.y < gy0) gy0 = base + g.y;
      if (base + g.y + g.h > gy1) gy1 = base + g.y + g.h;
    }
    xx += g.advance;
  }
  if (gx1 <= gx0) return;
  // footprint: the glyphs grown by the ring (2 px) and the shade (1 px down)
  int x0 = gx0 - 2, y0 = gy0 - 2, x1 = gx1 + 2, y1 = gy1 + 3;
  if (!claim(x0, y0, x1, y1, Hash(12).s(t).u(x).u(base).p(&f).u(col).u(edge).v)) return;
  // scratch: the glyph alpha over [gx0-2, gx1+2) x [gy0-2, gy1+3), zero-padded
  const int sw = gx1 - gx0 + 4, sh = gy1 - gy0 + 5;
  if (!g_scratch || sw * sh > Retained::kScratch) return;
  memset(g_scratch, 0, (size_t)(sw * sh));
  xx = x;
  for (const char* q = t; *q; ++q) {
    const unsigned ch = (unsigned char)*q;
    if (ch < 32 || ch >= 127) continue;
    const doodle_art::Glyph& g = f.glyphs[ch - 32];
    const int ox = xx + g.x - (gx0 - 2), oy = base + g.y - (gy0 - 2);
    for (int r = 0; r < g.h; ++r) {
      const uint8_t* src = f.alpha + g.offset + r * g.w;
      uint8_t* dst = g_scratch + (oy + r) * sw + ox;
      for (int c = 0; c < g.w; ++c)
        if (src[c] > dst[c]) dst[c] = src[c];
    }
    xx += g.advance;
  }
  const uint16_t shadeCol = shade(col, 0.7f);
  auto at = [&](int px_, int py_) -> unsigned {             // scratch alpha at screen (px_, py_), 0 outside
    const int sx = px_ - (gx0 - 2), sy = py_ - (gy0 - 2);
    return (sx < 0 || sy < 0 || sx >= sw || sy >= sh) ? 0u : g_scratch[sy * sw + sx];
  };
  static const int8_t kRing[8][2] = {{-2, 0}, {2, 0}, {0, -2}, {0, 2}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
  for (int y = y0; y < y1; ++y) {
    uint16_t* d = g_px + y * kW;
    for (int xq = x0; xq < x1; ++xq) {
      unsigned e = 0;
      for (const auto& o : kRing) {
        const unsigned v = at(xq - o[0], y - o[1]);
        if (v > e) e = v;
      }
      if (!e) continue;
      const unsigned sa = at(xq, y - 1), fa = at(xq, y);
      uint16_t c = swap16(d[xq]);
      c = mix(edge, c, e);
      if (sa) c = mix(shadeCol, c, sa);
      if (fa) c = mix(col, c, fa);
      ++g_blends;
      d[xq] = swap16(c);
    }
  }
}

// ---- the scene ----

int pinSprite(const B::Pin& p, bool* mirror) {
  *mirror = false;
  if (p.state == B::PinState::Standing || p.tilt < 22) return 0;
  const int t = p.tilt < 67 ? 0 : 1;
  float deg = p.dir * 180.0f / kPi;
  if (deg > 90) { deg = 180 - deg; *mirror = true; }
  else if (deg < -90) { deg = -180 - deg; *mirror = true; }
  int di = (int)lroundf((90 - deg) / 45);
  di = di < 0 ? 0 : (di > 4 ? 4 : di);
  return 1 + t * 5 + di;
}

void aimLine(float x0, float deg) {
  const float a = deg * kPi / 180.0f, tx = sinf(a), ty = cosf(a);
  for (int i = 0; i < 26; ++i) {
    const float dist = 1.2f + (i + 0.5f) * 0.62f;
    const float x = x0 + tx * dist, y = ty * dist;
    if (y > B::kLaneLen - 1.2f || fabsf(x) > B::kLaneHalfW) break;
    float sx, sy;
    if (!proj(kCamA, x, y, 0.004f, &sx, &sy)) continue;
    const float k = kCamA.f / (y - kCamA.y);
    softEllipse(sx, sy, fmaxf(0.9f, 0.045f * k), fmaxf(0.6f, 0.02f * k), rgb(255, 236, 120), 210);
  }
  // an arrowhead at the far end of the line
  const float dist = fminf(16.5f, (B::kLaneLen - 1.3f) / ty);
  float sx, sy, bx, by;
  if (proj(kCamA, x0 + tx * dist, ty * dist, 0, &sx, &sy) && proj(kCamA, x0 + tx * (dist - 1.2f), ty * (dist - 1.2f), 0, &bx, &by)) {
    for (int k = -3; k <= 3; ++k) {
      const float f = k / 3.0f;
      softEllipse(bx + f * 5.0f + (sx - bx) * (1 - fabsf(f)), by + (sy - by) * (1 - fabsf(f)), 1.0f, 0.8f,
                  rgb(255, 236, 120), 220);
    }
  }
}

void drawBowler(const B::Match& m, const Scene& s) {
  const int bi = s.pose == Pose::FollowThrough ? 1 : 0;
  const doodle_art::Sprite& spr = A::kBowler[bi];
  const int8_t* an = A::kBowlerAnchors[bi];
  const float d = A::kBowlerY - kCamA.y, ppm = kCamA.f / d;
  const float shX = kCamA.cx + m.startX() * ppm;             // the right shoulder is over the ball's line
  const float feetY = kCamA.hy + kCamA.h * ppm;
  const int x0 = (int)lroundf(shX) - an[2], y0 = (int)lroundf(feetY) - an[1];
  const float rsx = x0 + an[2], rsy = y0 + an[3], lsx = x0 + an[4], lsy = y0 + an[5];
  const float zs = kCamA.h - (rsy - kCamA.hy) / ppm;          // shoulder height (m)
  const uint16_t kShirt = rgb(232, 74, 60), kSkin = rgb(242, 201, 160);
  const float armR = 3.6f;
  // right hand / ball position in the world
  float hx, hy, hz;
  if (s.pose == Pose::Stance) {
    hx = m.startX() + 0.03f; hy = A::kBowlerY + 0.18f; hz = zs - 0.28f;     // ball held in front, both hands
  } else {
    const float ph = s.armDeg * kPi / 180.0f;
    hx = m.startX(); hy = A::kBowlerY - A::kArmLen * sinf(ph); hz = zs - A::kArmLen * cosf(ph);
  }
  float hsx, hsy;
  proj(kCamA, hx, hy, hz, &hsx, &hsy);
  const float br = B::kBallR * kCamA.f / (hy - kCamA.y);
  const bool front = hy > A::kBowlerY + 0.03f;                // hand beyond the body: the body hides it
  auto rightArm = [&]() {
    limb(rsx, rsy, hsx, hsy, armR, kShirt, kSkin, 0.35f);
    softEllipse(hsx, hsy, 3.2f, 3.2f, kSkin, 255);
    if (s.ballInHand) drawBall(hsx + (s.pose == Pose::Stance ? 0 : 1.0f), hsy + br * 0.35f, br, 0.6f, 255);
  };
  // left arm: to the ball at address; out to the side for balance while swinging
  float lhx, lhy;
  const float lsX = m.startX() - (an[2] - an[4]) / ppm;
  if (s.pose == Pose::Stance) proj(kCamA, lsX + 0.14f, A::kBowlerY + 0.24f, zs - 0.18f, &lhx, &lhy);
  else proj(kCamA, lsX - 0.30f, A::kBowlerY + 0.02f, zs - (s.pose == Pose::FollowThrough ? 0.12f : 0.26f), &lhx, &lhy);
  if (s.pose == Pose::Stance) limb(lsx, lsy, lhx, lhy, armR, kShirt, kSkin, 0.35f);
  if (front) rightArm();
  blit(spr, x0, y0);
  if (s.pose != Pose::Stance) {
    limb(lsx, lsy, lhx, lhy, armR, kShirt, kSkin, 0.35f);
    softEllipse(lhx, lhy, 3.0f, 3.0f, kSkin, 255);
  }
  if (!front) rightArm();
}

void drawApproach(const B::Match& m, const Scene& s) {
  const B::Lane& L = m.lane();
  static const uint8_t kOrder[10] = {6, 7, 8, 9, 3, 4, 5, 1, 2, 0};    // back row first
  item(kIPins);
  for (int k = 0; k < 10; ++k) {
    const B::Pin& p = L.pin(kOrder[k]);
    if (p.state != B::PinState::Standing) continue;
    float sx, sy;
    if (proj(kCamA, p.x, p.y, 0, &sx, &sy)) blit(A::kPinFar, (int)lroundf(sx) - A::kPinFarAx, (int)lroundf(sy) - A::kPinFarAy);
  }
  if (m.phase() == B::Phase::Aiming && s.pose != Pose::FollowThrough) { item(kIAim); aimLine(m.startX(), m.aimDeg()); }
  const B::Ball& b = L.ball();
  if (m.phase() == B::Phase::Rolling && b.live && !b.gone) {
    item(kILaneBall);
    const float z = b.gutter ? B::kBallR - 0.06f : B::kBallR;
    float sx, sy, gx, gy;
    if (proj(kCamA, b.x, b.y, z, &sx, &sy) && proj(kCamA, b.x, b.y, 0, &gx, &gy)) {
      const float r = B::kBallR * kCamA.f / (b.y - kCamA.y);
      if (!b.gutter) drawBall(sx, 2 * gy - sy, r, -b.roll / B::kBallR, 35, false);   // its reflection in the lane
      shadow(gx, gy, r * 1.1f, r * 0.35f, 120);
      drawBall(sx, sy, r, b.roll / B::kBallR, 255);
    }
  }
  item(kIBowler);
  drawBowler(m, s);
}

void drawDeck(const B::Match& m, const Scene& s) {
  const B::Lane& L = m.lane();
  struct Item { float y; int8_t pin; };
  Item items[11];
  int n = 0;
  for (int i = 0; i < B::kPins; ++i) {
    const B::Pin& p = L.pin(i);
    if (p.state == B::PinState::Gone || p.y - kCamD.y < 0.6f) continue;
    items[n++] = Item{p.y, (int8_t)i};
  }
  const B::Ball& b = L.ball();
  const bool ballShown = b.live && !b.gone && b.y - kCamD.y > 0.7f;
  if (ballShown) items[n++] = Item{b.y, -1};
  for (int i = 1; i < n; ++i)                                  // far first
    for (int j = i; j > 0 && items[j].y > items[j - 1].y; --j) { const Item t = items[j]; items[j] = items[j - 1]; items[j - 1] = t; }
  const float wob = s.nowMs * 0.045f;
  // reflections in the glossy deck, then soft shadows, then the pins and the ball, far to near
  for (int k = 0; k < n; ++k) {
    float sx, sy;
    item(items[k].pin < 0 ? (int)kIDeckBall : kIDeckPin0 + items[k].pin);
    if (items[k].pin < 0) {
      if (b.gutter) continue;
      float cx, cy;
      proj(kCamD, b.x, b.y, 0, &sx, &sy);
      proj(kCamD, b.x, b.y, B::kBallR, &cx, &cy);
      drawBall(cx, 2 * sy - cy, B::kBallR * kCamD.f / (b.y - kCamD.y), -b.roll / B::kBallR, 30, false);
      continue;
    }
    const B::Pin& p = L.pin(items[k].pin);
    if (p.out) continue;
    bool mir;
    const int si = pinSprite(p, &mir);
    proj(kCamD, p.x, p.y, 0, &sx, &sy);
    blitScaled(A::kPin[si], A::kPinAx[si], A::kPinAy[si], sx, sy, A::kPinD0 / (p.y - kCamD.y), mir, true, 70);
  }
  for (int k = 0; k < n; ++k) {
    float sx, sy;
    const float y = items[k].y;
    const float sc = kCamD.f / (y - kCamD.y);
    item(items[k].pin < 0 ? (int)kIDeckBall : kIDeckPin0 + items[k].pin);
    if (items[k].pin < 0) {
      proj(kCamD, b.x, b.y, 0, &sx, &sy);
      shadow(sx, sy, B::kBallR * sc * 1.05f, B::kBallR * sc * 0.28f, 130);
    } else {
      const B::Pin& p = L.pin(items[k].pin);
      if (p.out) continue;
      proj(kCamD, p.x, p.y, 0, &sx, &sy);
      if (p.state == B::PinState::Standing) shadow(sx, sy, 0.075f * sc, 0.022f * sc, 110);
      else shadow(sx, sy, 0.11f * sc, 0.03f * sc, 70);
    }
  }
  for (int k = 0; k < n; ++k) {
    float sx, sy;
    item(items[k].pin < 0 ? (int)kIDeckBall : kIDeckPin0 + items[k].pin);
    if (items[k].pin < 0) {
      const float z = b.gutter ? B::kBallR - 0.06f : B::kBallR;
      proj(kCamD, b.x, b.y, z, &sx, &sy);
      drawBall(sx, sy, B::kBallR * kCamD.f / (b.y - kCamD.y), b.roll / B::kBallR, 255);
      continue;
    }
    const B::Pin& p = L.pin(items[k].pin);
    bool mir;
    const int si = pinSprite(p, &mir);
    // a pin knocked hard flies a little before it lands: lift it with its speed
    const float sp = sqrtf(p.vx * p.vx + p.vy * p.vy);
    const float lift = p.state == B::PinState::Standing || p.out ? 0.0f : clampf((sp - 1.5f) * 0.035f, 0.0f, 0.14f);
    proj(kCamD, p.x, p.y, p.out ? -0.05f : lift, &sx, &sy);
    if (p.wobble > 0 && p.state == B::PinState::Standing) sx += sinf(wob + items[k].pin) * 1.6f * p.wobble;
    blitScaled(A::kPin[si], A::kPinAx[si], A::kPinAy[si], sx, sy, A::kPinD0 / (p.y - kCamD.y), mir, false, 255);
  }
}

// ---- HUD ----

void scorecard(const B::Scorecard& c, int y) {
  const int bw = 25, h = 25;
  const int cur = c.over() ? -1 : c.frame();
  panel(0, y, kW, h, rgb(248, 248, 244), 255, 3);          // opaque: a plain write per pixel, no blend
  for (int f = 0; f < 10; ++f) {
    const int x = f * bw;
    if (f == cur) fillRect(x + 1, y + 1, bw - 1, h - 2, rgb(255, 214, 90), 150);
    if (f) fillRect(x, y + 2, 1, h - 4, rgb(120, 120, 130), 200);
    fillRect(x + 1, y + 12, bw - 1, 1, rgb(190, 190, 200), 200);
    char mk[4];
    c.marks(f, mk);
    const int nm = (int)strlen(mk);
    for (int k = 0; k < nm; ++k) {
      const char ch[2] = {mk[k], 0};
      const int cx = f == 9 ? x + 5 + k * 8 : (nm == 1 && mk[0] == 'X' ? x + 17 : x + 6 + k * 11);
      const uint16_t col = mk[k] == 'X' ? rgb(210, 40, 40) : (mk[k] == '/' ? rgb(40, 90, 200) : rgb(40, 40, 50));
      text(ch, cx, y + 10, doodle_art::font11, col, 1);
      int qx0 = cx - 7, qy0 = y - 1, qx1 = cx + 8, qy1 = y + 14;
      if (c.splitAt(f, k) && claim(qx0, qy0, qx1, qy1, Hash(10).u(cx).u(y).v)) {   // a split: circle the count
        for (int a = 0; a < 20; ++a) {
          const float t = a * kPi / 10;
          px((int)lroundf(cx + cosf(t) * 5.0f), (int)lroundf(y + 6 + sinf(t) * 5.0f), rgb(210, 40, 40), 200);
        }
      }
    }
    const int tot = c.total(f);
    if (tot >= 0) {
      char buf[6];
      snprintf(buf, sizeof(buf), "%d", tot);
      text(buf, x + bw / 2, y + 23, doodle_art::font11, rgb(30, 30, 40), 1);
    }
  }
  fillRect(250, y + 2, 1, h - 4, rgb(80, 80, 90), 230);
  char buf[6];
  snprintf(buf, sizeof(buf), "%d", c.score());
  text(buf, 265, y + 18, doodle_art::font13, rgb(30, 30, 40), 1);
}

void pinDiagram(uint16_t rack, uint16_t standing, int x, int y) {
  panel(x - 3, y - 3, 40, 34, rgb(16, 24, 44), 170, 4);
  for (int i = 0; i < 10; ++i) {
    int row = 0;
    while ((row + 1) * (row + 2) / 2 <= i) ++row;
    const int j = i - row * (row + 1) / 2;
    const float cx = x + 17 + (j - row / 2.0f) * 9.0f, cy = y + 25 - row * 7.5f;
    const bool up = (standing >> i) & 1, was = (rack >> i) & 1;
    if (up) softEllipse(cx, cy, 3.0f, 3.0f, rgb(255, 255, 255), 255);
    else softEllipse(cx, cy, 2.6f, 2.6f, was ? rgb(120, 130, 150) : rgb(70, 78, 96), 200);
  }
}

void bottomHint(const char* msg) {
  const int w = textW(msg, doodle_art::font11) + 12;
  panel(kW - w - 4, 219, w, 17, rgb(16, 24, 44), 180, 4);
  text(msg, kW - 10, 231, doodle_art::font11, rgb(255, 255, 255), 2);
}

void centerPanel(const char* msg, int y) {
  const int w = textW(msg, doodle_art::font16) + 24;
  panel(kW / 2 - w / 2, y, w, 28, rgb(16, 24, 44), 205, 6);
  text(msg, kW / 2, y + 19, doodle_art::font16, rgb(255, 255, 255), 1);
}

void sparkle(float cx, float cy, float r, uint16_t col, unsigned a) {
  int x0 = (int)cx - (int)r - 1, y0 = (int)cy - (int)r - 1, x1 = (int)cx + (int)r + 2, y1 = (int)cy + (int)r + 2;
  const bool paint = claim(x0, y0, x1, y1, Hash(11).f(cx).f(cy).f(r).u(col).u(a).v);
  softEllipse(cx, cy, 1.5f, 1.5f, col, a);
  if (!paint) return;
  for (int k = -(int)r; k <= (int)r; ++k) {
    const unsigned f = (unsigned)(a * (1 - fabsf((float)k) / (r + 1)));
    px((int)cx + k, (int)cy, col, f);
    px((int)cx, (int)cy + k, col, f);
  }
}

void banner(const B::Match& m, const Scene& s) {
  const char* t = nullptr;
  uint16_t col = rgb(255, 255, 255), edge = rgb(20, 30, 60);
  char buf[24];
  switch (m.outcome()) {
    case B::Outcome::Strike: t = "STRIKE!"; col = rgb(255, 220, 60); edge = rgb(150, 40, 20); break;
    case B::Outcome::Spare: t = "SPARE!"; col = rgb(120, 210, 255); edge = rgb(20, 40, 110); break;
    case B::Outcome::Split: t = "SPLIT"; col = rgb(255, 120, 120); edge = rgb(90, 20, 40); break;
    case B::Outcome::Gutter: t = "GUTTER"; col = rgb(200, 200, 210); edge = rgb(40, 40, 50); break;
    default:
      snprintf(buf, sizeof(buf), m.lastCount() == 1 ? "1 pin" : "%d pins", m.lastCount());
      t = buf;
      break;
  }
  const uint32_t age = s.resultMs;
  const int pw = textW(t, doodle_art::font30) + 70, slide = 0;
  panel(kW / 2 - pw / 2, 92, pw, 44, rgb(10, 16, 34), 175, 6);
  fillRect(kW / 2 - pw / 2 + 6, 92, pw - 12, 2, col, 200);
  fillRect(kW / 2 - pw / 2 + 6, 134, pw - 12, 2, col, 200);
  bigText(t, kW / 2 + slide, 125, doodle_art::font30, col, edge);
  if (m.outcome() == B::Outcome::Strike) {
    // sparkles twinkle in place around the word (moving ones would repaint the whole banner every frame)
    static const int8_t kAt[6][2] = {{-78, -12}, {-58, 10}, {-36, -18}, {40, 12}, {62, -16}, {82, 6}};
    for (int k = 0; k < 6; ++k) {
      const uint32_t ph = (age / 83 + (uint32_t)k * 2) % 6;          // one step per frame
      const float r = ph < 3 ? 2.0f + ph * 1.5f : 2.0f + (6 - ph) * 1.5f;
      item(kISpark0 + k);                                       // each its own item: a small repaint
      sparkle(kW / 2 + kAt[k][0], 114 + kAt[k][1], r, rgb(255, 250, 200), 230);
    }
    item(kIBanner);
  }
}

void gameOver(const B::Match& m, const Scene& s) {
  panel(40, 88, 200, 104, rgb(10, 16, 34), 215, 8);
  fillRect(52, 118, 176, 1, rgb(255, 214, 90), 200);
  text("GAME OVER", kW / 2, 112, doodle_art::font16, rgb(255, 214, 90), 1);
  char buf[32];
  snprintf(buf, sizeof(buf), "%d", m.card().score());
  bigText(buf, kW / 2, 158, doodle_art::font30, rgb(255, 255, 255), rgb(20, 30, 60));
  snprintf(buf, sizeof(buf), "best %d", s.best);
  text(buf, kW / 2, 174, doodle_art::font11, rgb(190, 200, 220), 1);
  text("press the dial to play again", kW / 2, 187, doodle_art::font11, rgb(255, 255, 255), 1);
}

// ---- how to play (hold A) ----
// Lines starting with '#' are the page's heading. Keep every line within kHelpTextW (test_bowling_view checks).
const char* const kHelp[games::bowling_help::kPages][9] = {
    {"#Throwing",
     "Press and hold B: that pose is your start.",
     "A / B: step left / right. The dial aims.",
     "Hold B, swing your arm back, then forward,",
     "and let go of B to throw. (Keep holding and",
     "it lets go at the end of the forward swing.)",
     "A relaxed swing is a normal speed;",
     "swing harder for a faster ball.", nullptr},
    {"#Line and hook",
     "Swing straight back and through to throw",
     "straight; across your body angles the ball.",
     "Twist your wrist as you swing through to",
     "hook it: palm turning left = hook left.",
     "For strikes, aim for the pocket just right",
     "of the head pin, with a little hook.",
     "Hitting the head pin full on leaves splits.", nullptr},
    {"#Scoring",
     "The strip at the top is your scorecard;",
     "the running total is on the right.",
     "10 frames, 2 balls each (3 in 10th after",
     "a strike or spare). X strike: 10 + your next",
     "2 balls. / spare: 10 + your next ball.",
     "A circled count is a split.",
     "A+B exits. Mid-swing, A+B is ignored so a",
     "slip of the finger can't end your game."},
};
constexpr int kHelpX = 14, kHelpY = 44, kHelpW = 252, kHelpH = 190;

// The card is opaque, so a page turn repaints only the text on it (see g_onlyHelp); a translucent card would have
// repainted the scene under it on every turn.
Rect helpInner() { return Rect{kHelpX + 10, kHelpY + 10, kHelpX + kHelpW - 10, kHelpY + kHelpH - 10}; }

void helpPanel(int page) {
  if (page < 0 || page >= games::bowling_help::kPages) return;
  item(kIHelp);
  const uint16_t rim = rgb(120, 200, 255);                                               // a thin glowing rim
  fillRect(kHelpX + 8, kHelpY - 2, kHelpW - 16, 2, rim, 150);
  fillRect(kHelpX + 8, kHelpY + kHelpH, kHelpW - 16, 2, rim, 150);
  fillRect(kHelpX - 2, kHelpY + 8, 2, kHelpH - 16, rim, 150);
  fillRect(kHelpX + kHelpW, kHelpY + 8, 2, kHelpH - 16, rim, 150);
  panel(kHelpX, kHelpY, kHelpW, kHelpH, rgb(16, 26, 56), 255, 10);
  fillRect(kHelpX + 10, kHelpY + 4, kHelpW - 20, 26, rgb(26, 42, 86), 255);             // title band
  fillRect(kHelpX + 12, kHelpY + 30, kHelpW - 24, 1, rgb(120, 200, 255), 255);
  text("How to play: Bowling", kW / 2, kHelpY + 22, doodle_art::font16, rgb(255, 255, 255), 1);
  item(kIHelpText);
  int y = kHelpY + 50;
  for (const char* line : kHelp[page]) {
    if (!line) break;
    if (*line == '#') {
      text(line + 1, kHelpX + 12, y, doodle_art::font13, rgb(255, 214, 90));
      y += 17;
    } else {
      text(line, kHelpX + 12, y, doodle_art::font11, rgb(235, 240, 250));
      y += 13;
    }
  }
  // page dots and how to leave
  const int n = games::bowling_help::kPages, dy = kHelpY + kHelpH - 20;
  for (int i = 0; i < n; ++i)
    softEllipse(kW / 2 - (n - 1) * 6 + i * 12, dy, 3.0f, 3.0f, i == page ? rgb(255, 214, 90) : rgb(90, 104, 130), 255);
  text("dial: page", kHelpX + 12, dy + 4, doodle_art::font11, rgb(150, 170, 200));
  text("tap: close", kHelpX + kHelpW - 12, dy + 4, doodle_art::font11, rgb(150, 170, 200), 2);
}

bool g_onlyHelp = false;                                       // paint pass: this rectangle is all opaque card

// The whole picture, in order. Run once to measure and once per dirty rectangle to paint.
void drawScene(const B::Match& m, const Scene& s) {
  if (s.view == View::Deck) drawDeck(m, s);
  else drawApproach(m, s);
}

void drawHud(const B::Match& m, const Scene& s) {
  if (g_onlyHelp) { helpPanel(s.helpPage - 1); return; }
  const bool over = m.phase() == B::Phase::GameOver;
  item(kIScore);
  if (s.view == View::Approach || s.resultMs || over) scorecard(m.card(), 39);
  if (s.view == View::Approach && !over) {
    item(kIDiagram);
    pinDiagram(m.rack(), m.lane().standing(), 8, 202);
  }
  item(kIHint);
  if (s.hint && *s.hint && !s.helpPage) bottomHint(s.hint);
  item(kISpeed);
  if (s.speedMph > 0 && !over) {
    char buf[16];
    snprintf(buf, sizeof(buf), "%.0f mph", s.speedMph);
    const int y = s.view == View::Approach || s.resultMs ? 66 : 42;
    panel(kW - 62, y, 58, 18, rgb(16, 24, 44), 170, 4);
    text(buf, kW - 33, y + 13, doodle_art::font11, rgb(255, 255, 255), 1);
  }
  item(kIBanner);
  if (s.resultMs && !over) banner(m, s);
  item(kIOver);
  if (over) gameOver(m, s);
  item(kICenter);
  if (s.center && *s.center && !s.helpPage) centerPanel(s.center, 100);
  if (s.helpPage > 0) helpPanel(s.helpPage - 1);
}

using games::dirty::area;
using games::dirty::mergeRects;

}  // namespace

void invalidate(Retained& r) { r.valid = false; }

int helpTextBottom() {                                           // lowest text pixel on any page
  int lowest = 0;
  for (const auto& page : kHelp) {
    int y = kHelpY + 50, last = y;
    for (const char* line : page) {
      if (!line) break;
      last = y;
      y += *line == '#' ? 17 : 13;
    }
    if (last + 3 > lowest) lowest = last + 3;
  }
  return lowest;
}

int helpWidestLine() {
  int widest = 0;
  for (const auto& page : kHelp)
    for (const char* line : page) {
      if (!line) break;
      const int w = *line == '#' ? textW(line + 1, doodle_art::font13) : textW(line, doodle_art::font11);
      if (w > widest) widest = w;
    }
  return widest;
}

void draw(uint16_t* canvas, const B::Match& m, const Scene& s, Retained* R, Timing* tm) {
  g_px = canvas;
  g_scratch = R ? R->scratch : nullptr;
  const uint32_t t0 = tm ? tm->clockUs() : 0;
  const bool full = !R || !R->valid || R->canvas != canvas || R->view != (uint8_t)s.view ||
                    (int32_t)(s.nowMs - R->fullAt) >= (int32_t)Retained::kFullMs;
  // 1) measure (skipped when there is nothing to compare with)
  Rect rects[Retained::kMaxRects + 1];
  int n = 0;
  if (R) {
    for (int i = 0; i < kItemCount; ++i) {
      R->cur[i] = Rect{kW, kH, 0, 0};
      R->curSig[i] = 2166136261u;
    }
    g_ret = R;
    g_mode = Mode::Measure;
    drawScene(m, s);
    drawHud(m, s);
    g_ret = nullptr;
    for (int i = 0; i < kItemCount; ++i)
      if (R->cur[i].x1 <= R->cur[i].x0) R->cur[i] = Rect{0, 0, 0, 0};
  }
  // 2) what changed: old and new footprints of every item that changed
  if (full) {
    rects[n++] = Rect{0, kTop, kW, kH};
  } else {
    for (int i = 0; i < kItemCount; ++i) {
      const Rect& nb = R->cur[i];
      const Rect& ob = R->box[i];
      if (R->curSig[i] == R->sig[i] && nb.x0 == ob.x0 && nb.y0 == ob.y0 && nb.x1 == ob.x1 && nb.y1 == ob.y1) continue;
      for (const Rect* r : {&ob, &nb}) {
        if (r->x1 <= r->x0 || r->y1 <= r->y0) continue;
        rects[n++] = *r;
        if (n > Retained::kMaxRects) n = mergeRects(rects, n, Retained::kMaxRects);
      }
    }
    n = mergeRects(rects, n, Retained::kMaxRects);
  }
  const uint32_t t1 = tm ? tm->clockUs() : 0;
  // 3) background back where things changed
  const uint16_t* bg = s.view == View::Deck ? A::kDeckBg : A::kApproachBg;
  uint32_t px = 0;
  const Rect inner = helpInner();
  auto underCard = [&](const Rect& r) {
    return s.helpPage > 0 && r.x0 >= inner.x0 && r.y0 >= inner.y0 && r.x1 <= inner.x1 && r.y1 <= inner.y1;
  };
  for (int k = 0; k < n; ++k) {
    const Rect& r = rects[k];
    if (underCard(r)) continue;                                // the opaque card repaints it whole
    const int w = r.x1 - r.x0;
    for (int y = r.y0; y < r.y1; ++y)
      memcpy(canvas + y * kW + r.x0, bg + (y - kTop) * kW + r.x0, (size_t)w * 2);
    px += (uint32_t)area(r);
  }
  const uint32_t t2 = tm ? tm->clockUs() : 0;
  // 4) paint, clipped to each rectangle
  g_mode = Mode::Paint;
  g_blends = 0;
  uint32_t sceneUs = 0, hudUs = 0;
  for (int k = 0; k < n; ++k) {
    g_cx0 = rects[k].x0; g_cy0 = rects[k].y0; g_cx1 = rects[k].x1; g_cy1 = rects[k].y1;
    const uint32_t a = tm ? tm->clockUs() : 0;
    g_onlyHelp = underCard(rects[k]);
    if (!g_onlyHelp) drawScene(m, s);
    const uint32_t b = tm ? tm->clockUs() : 0;
    drawHud(m, s);
    g_onlyHelp = false;
    const uint32_t c = tm ? tm->clockUs() : 0;
    sceneUs += b - a;
    hudUs += c - b;
  }
  g_cx0 = 0; g_cy0 = kTop; g_cx1 = kW; g_cy1 = kH;
  // 5) remember this frame
  if (R) {
    for (int i = 0; i < kItemCount; ++i) {
      R->box[i] = R->cur[i];
      R->sig[i] = R->curSig[i];
    }
    R->canvas = canvas;
    R->view = (uint8_t)s.view;
    if (full) R->fullAt = s.nowMs;
    R->valid = true;
    R->rects = (uint8_t)n;
    for (int k = 0; k < n; ++k) R->rect[k] = rects[k];
  }
  if (tm) {
    tm->measureUs = t1 - t0;
    tm->bgUs = t2 - t1;
    tm->sceneUs = sceneUs;
    tm->hudUs = hudUs;
    tm->rects = (uint8_t)n;
    tm->area = px;
    tm->blends = g_blends;
  }
}

}  // namespace bowling_view
}  // namespace games
