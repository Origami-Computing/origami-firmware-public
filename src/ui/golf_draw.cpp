// Golf screens: see include/ui/golf_draw.h.
#include "ui/golf_draw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "assets/doodle_notebook.h"
#include "assets/golf_art.h"
#include "core/golf.h"
#include "ui/golf_help.h"

namespace ui {
namespace golf {
using namespace core::golf;

namespace gfx {

namespace {
inline uint16_t swap16(uint16_t v) { return (uint16_t)((v >> 8) | (v << 8)); }
// RGB565 packed blend: the channels spread into one 32-bit word, 5-bit alpha (0..32).
inline uint32_t expand(uint16_t c) { return (c | ((uint32_t)c << 16)) & 0x07E0F81Fu; }
inline uint16_t mixPacked(uint32_t fg32, uint16_t bg, uint32_t a32) {
  const uint32_t b = expand(bg);
  const uint32_t r = ((((fg32 - b) * a32) >> 5) + b) & 0x07E0F81Fu;
  return (uint16_t)(r | (r >> 16));
}
inline uint32_t alpha32(unsigned a) { return (a + 4) >> 3; }
const doodle_art::Font& font(int size) {
  switch (size) {
    case 11: return doodle_art::font11;
    case 25: return doodle_art::font25;
    default: return doodle_art::font13;           // 16 and 30 map to 13 and 25 (each font costs flash)
  }
}
inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// A horizontal run of one colour at one alpha (clipped).
void hspan(uint16_t* c, int y, int x0, int x1, uint16_t col, uint8_t a) {
  if (!a || y < kTop || y >= kScreenH) return;
  if (x0 < 0) x0 = 0;
  if (x1 > kScreenW) x1 = kScreenW;
  if (x0 >= x1) return;
  uint16_t* row = c + y * kScreenW;
  if (a >= 252) {
    const uint16_t v = swap16(col);
    for (int x = x0; x < x1; ++x) row[x] = v;
    return;
  }
  const uint32_t fg = expand(col), a5 = alpha32(a);
  for (int x = x0; x < x1; ++x) row[x] = swap16(mixPacked(fg, swap16(row[x]), a5));
}
}  // namespace

void blend(uint16_t* c, int x, int y, uint16_t col, uint8_t a) {
  if (!a || x < 0 || x >= kScreenW || y < kTop || y >= kScreenH) return;
  uint16_t& d = c[y * kScreenW + x];
  d = a >= 252 ? swap16(col) : swap16(mixPacked(expand(col), swap16(d), alpha32(a)));
}

void rect(uint16_t* c, int x, int y, int w, int h, uint16_t col, uint8_t a) {
  for (int yy = y; yy < y + h; ++yy) hspan(c, yy, x, x + w, col, a);
}

void panel(uint16_t* c, int x, int y, int w, int h, uint8_t a) {
  const uint16_t col = rgb(16, 28, 36);
  for (int yy = y; yy < y + h; ++yy) {
    const int ey = yy < y + 5 ? y + 5 - yy : (yy >= y + h - 5 ? yy - (y + h - 6) : 0);
    if (!ey) { hspan(c, yy, x, x + w, col, a); continue; }
    hspan(c, yy, x + 5, x + w - 5, col, a);
    for (int k = 0; k < 5; ++k) {                              // rounded corners
      const int ex = 5 - k, d2 = ex * ex + ey * ey;
      if (d2 > 30) continue;
      const uint8_t aa = d2 > 20 ? a / 2 : a;
      blend(c, x + k, yy, col, aa);
      blend(c, x + w - 1 - k, yy, col, aa);
    }
  }
  hspan(c, y, x + 5, x + w - 5, rgb(255, 255, 255), 40);      // a faint top highlight
}

void disc(uint16_t* c, float cx, float cy, float rx, float ry, uint16_t col, uint8_t a) {
  const int y0 = (int)floorf(cy - ry - 1), y1 = (int)ceilf(cy + ry + 1);
  for (int y = y0; y <= y1; ++y) {
    if (y < kTop || y >= kScreenH) continue;
    const float dy = (y + 0.5f - cy) / ry;
    const float rowCov = clamp01((1.0f - fabsf(dy)) * ry + 0.5f);
    if (rowCov <= 0) continue;
    const float half = rx * sqrtf(fmaxf(0.0f, 1.0f - dy * dy));
    const int in0 = (int)ceilf(cx - half + 0.5f), in1 = (int)floorf(cx + half - 0.5f);
    const uint8_t ar = (uint8_t)(a * rowCov);
    if (in1 >= in0) hspan(c, y, in0, in1, col, ar);
    // anti-aliased ends
    for (int x = (int)floorf(cx - half - 1); x < in0; ++x) {
      const float cov = clamp01(half + 0.5f - fabsf(x + 0.5f - cx));
      if (cov > 0) blend(c, x, y, col, (uint8_t)(ar * cov));
    }
    for (int x = in1 < in0 ? in0 : in1; x <= (int)ceilf(cx + half + 1); ++x) {
      if (x < in0 || (in1 >= in0 && x < in1)) continue;
      const float cov = clamp01(half + 0.5f - fabsf(x + 0.5f - cx));
      if (cov > 0) blend(c, x, y, col, (uint8_t)(ar * cov));
    }
  }
}

void ring(uint16_t* c, float cx, float cy, float rx, float ry, float width, uint16_t col, uint8_t a) {
  const float ox = rx + width, oy = ry + width, ix = rx - width - 1, iy = ry - width - 1;
  const float m = rx < ry ? rx : ry;
  const int y0 = (int)floorf(cy - oy), y1 = (int)ceilf(cy + oy);
  for (int y = y0; y <= y1; ++y) {
    if (y < kTop || y >= kScreenH) continue;
    const float dyp = y + 0.5f - cy;
    const float fo = 1.0f - dyp * dyp / (oy * oy);
    if (fo <= 0) continue;
    const float ho = ox * sqrtf(fo);
    const float fi = ix > 0 && iy > 0 ? 1.0f - dyp * dyp / (iy * iy) : -1.0f;
    const float hi = fi > 0 ? ix * sqrtf(fi) : -1.0f;       // pixels closer than this are inside the hole
    for (int x = (int)floorf(cx - ho); x <= (int)ceilf(cx + ho); ++x) {
      const float dx = x + 0.5f - cx;
      if (hi > 0 && fabsf(dx) < hi) {                        // jump over the hole (always forward)
        const int next = (int)ceilf(cx + hi - 0.5f);
        if (next - 1 > x) x = next - 1;
        continue;
      }
      const float k = sqrtf(dx * dx / (rx * rx) + dyp * dyp / (ry * ry));
      const float cov = clamp01(width * 0.5f + 0.5f - fabsf(k - 1.0f) * m);
      if (cov > 0) blend(c, x, y, col, (uint8_t)(cov * a));
    }
  }
}

void line(uint16_t* c, float x0, float y0, float x1, float y1, float width, uint16_t col, uint8_t a) {
  const float dx = x1 - x0, dy = y1 - y0, l2 = dx * dx + dy * dy;
  const float reach = width * 0.5f + 0.5f, reach2 = reach * reach;
  const int bx0 = (int)floorf(fminf(x0, x1) - width - 1), bx1 = (int)ceilf(fmaxf(x0, x1) + width + 1);
  const int by0 = (int)floorf(fminf(y0, y1) - width - 1), by1 = (int)ceilf(fmaxf(y0, y1) + width + 1);
  const float il2 = l2 > 0 ? 1.0f / l2 : 0;
  for (int y = by0; y <= by1; ++y) {
    if (y < kTop || y >= kScreenH) continue;
    for (int x = bx0; x <= bx1; ++x) {
      const float px = x + 0.5f - x0, py = y + 0.5f - y0;
      float t = (px * dx + py * dy) * il2;
      t = t < 0 ? 0 : (t > 1 ? 1 : t);
      const float ex = px - dx * t, ey = py - dy * t, e2 = ex * ex + ey * ey;
      if (e2 >= reach2) continue;
      const float cov = clamp01(reach - sqrtf(e2));
      if (cov > 0) blend(c, x, y, col, (uint8_t)(cov * a));
    }
  }
}

int textWidth(const char* s, int size) {
  const doodle_art::Font& f = font(size);
  int w = 0;
  for (; *s; ++s) {
    const unsigned ch = (unsigned char)*s;
    if (ch >= 32 && ch < 127) w += f.glyphs[ch - 32].advance;
  }
  return w;
}

static void glyphs(uint16_t* c, const char* s, int x, int baseline, const doodle_art::Font& f, uint16_t col, uint8_t alpha) {
  for (; *s; ++s) {
    const unsigned ch = (unsigned char)*s;
    if (ch < 32 || ch >= 127) continue;
    const doodle_art::Glyph& g = f.glyphs[ch - 32];
    for (int yy = 0; yy < g.h; ++yy)
      for (int xx = 0; xx < g.w; ++xx) {
        const uint8_t a = f.alpha[g.offset + yy * g.w + xx];
        if (a) blend(c, x + g.x + xx, baseline + g.y + yy, col, (uint8_t)(a * alpha / 255));
      }
    x += g.advance;
  }
}

void text(uint16_t* c, const char* s, int x, int baseline, int size, uint16_t col, int align, bool shadow) {
  const doodle_art::Font& f = font(size);
  const int w = textWidth(s, size);
  if (align == 1) x -= w / 2;
  else if (align == 2) x -= w;
  if (shadow) glyphs(c, s, x + 1, baseline + 1, f, rgb(0, 0, 0), 150);
  glyphs(c, s, x, baseline, f, col, 255);
}

}  // namespace gfx

using namespace gfx;

namespace {

const uint16_t kWhite = rgb(255, 255, 255), kGold = rgb(255, 214, 90), kSoft = rgb(200, 215, 225);
// Layout (play screens): the bottom strip holds every hint; the club / pin boxes sit above it; the feedback column
// on the left, under the hole panel.
constexpr int kStripY = 224, kBoxY = 190, kColY = kFeedbackY, kColH = kFeedbackH, kColW = kFeedbackW;

void sprite(uint16_t* c, const golf_art::Sprite& sp, int x, int y, bool flip = false) {
  for (int j = 0; j < sp.h; ++j)
    for (int i = 0; i < sp.w; ++i) {
      const int si = j * sp.w + (flip ? sp.w - 1 - i : i);
      if (sp.a[si]) blend(c, x + i, y + j, sp.px[si], sp.a[si]);
    }
}

void drawImage(uint16_t* c, const Scene& s) {
  const uint16_t bg = (uint16_t)((rgb(20, 40, 24) >> 8) | (rgb(20, 40, 24) << 8));
  for (int y = 0; y < kScreenH; ++y) {
    uint16_t* row = c + y * kScreenW;
    const int iy = s.camY + (y - kTop);
    if (y < kTop || !s.image || iy < 0 || iy >= s.imageH) {   // the status bar covers y < kTop
      for (int x = 0; x < kScreenW; ++x) row[x] = bg;
      continue;
    }
    const uint16_t* src = s.image + iy * s.imageW;
    int x = 0;
    for (; x < kScreenW && s.camX + x < 0; ++x) row[x] = bg;
    const int n = s.imageW - (s.camX + x) < kScreenW - x ? s.imageW - (s.camX + x) : kScreenW - x;
    if (n > 0) {
      memcpy(row + x, src + s.camX + x, (size_t)n * 2);
      x += n;
    }
    for (; x < kScreenW; ++x) row[x] = bg;
  }
}

inline float sxOf(const Scene& s, float wx) { return viewX(s.view, wx) - s.camX; }
inline float syOf(const Scene& s, float wy) { return viewY(s.view, wy) - s.camY + kTop; }

void drawFlagAt(uint16_t* c, const Scene& s, float wx, float wy, bool big) {
  const float px = sxOf(s, wx), py = syOf(s, wy);
  if (px < -30 || px > kScreenW + 30 || py < kTop - 50 || py > kScreenH + 10) return;
  const float cup = big ? cupRadiusPx(s.view.scale) : 1.3f;    // big (green view): the cup that counts
  disc(c, px, py, cup, cup, rgb(20, 30, 20), 230);
  const float pole = big ? 40.0f : 14.0f;
  line(c, px, py, px + pole * 0.55f, py + pole * 0.22f, big ? 2.0f : 1.2f, rgb(0, 0, 0), 70);   // shadow
  const golf_art::Sprite& f = golf_art::kFlags[(big ? 3 : 0) + (s.frame / 3) % 3];
  sprite(c, f, (int)lroundf(px) - f.ax, (int)lroundf(py) - f.ay);
}

void drawBall(uint16_t* c, const Scene& s) {
  const float gx = sxOf(s, s.ballX), gy = syOf(s, s.ballY);
  const float scale = s.view.scale;
  const float base = s.bigFlag ? fmaxf(2.0f, scale * 0.35f) : 1.7f;
  const float h = s.ballH > 0 ? s.ballH : 0;
  const float r = base * (1.0f + h / 28.0f);
  if (gy > kScreenH + 1) {                         // below the screen: a marker at the bottom edge
    const float x = gx < 8 ? 8 : (gx > kScreenW - 8 ? kScreenW - 8 : gx);
    disc(c, x, kScreenH - 22, 4.5f, 4.5f, rgb(16, 28, 36), 200);
    disc(c, x, kScreenH - 22, 2.5f, 2.5f, rgb(250, 250, 248), 255);
    return;
  }
  const float lift = h * fmaxf(scale, 0.9f) * 0.9f;
  disc(c, gx + h * 0.25f * scale + 0.6f, gy + 0.6f, r * 1.2f, r * 0.75f, rgb(0, 0, 0), (uint8_t)(h > 1 ? 90 : 120));
  const float bx = gx, by = gy - lift;
  disc(c, bx, by, r + 0.7f, r + 0.7f, rgb(40, 50, 40), 200);
  disc(c, bx, by, r, r, rgb(250, 250, 248), 255);
  if (r > 1.8f) disc(c, bx - r * 0.3f, by - r * 0.3f, r * 0.4f, r * 0.4f, rgb(255, 255, 255), 200);
}

void drawAim(uint16_t* c, const Scene& s) {
  const float a = s.aimDeg * 0.0174533f;
  const float dx = sinf(a), dy = -cosf(a);
  const float x0 = sxOf(s, s.ballX), y0 = syOf(s, s.ballY);
  const float len = s.aimYd * s.view.scale;
  for (float t = 7; t < len - 5; t += 7) {
    const float px = x0 + dx * t, py = y0 + dy * t;
    if (py > kScreenH + 2) continue;
    const uint8_t al = (uint8_t)(235 - 90 * t / (len + 1));
    disc(c, px, py, 1.9f, 1.9f, rgb(20, 40, 20), (uint8_t)(al / 2));
    disc(c, px, py, 1.3f, 1.3f, kWhite, al);
  }
  const float mx = x0 + dx * len, my = y0 + dy * len;
  const float rr = fmaxf(4.0f, s.aimYd * 0.05f * s.view.scale);
  disc(c, mx, my, rr, rr * 0.8f, kWhite, 45);
  ring(c, mx, my, rr, rr * 0.8f, 1.4f, kWhite, 220);
  line(c, mx - 2.5f, my, mx + 2.5f, my, 1.2f, kWhite, 220);
  line(c, mx, my - 2.5f, mx, my + 2.5f, 1.2f, kWhite, 220);
  if (s.markerYd >= 0) {
    const float l2 = s.markerYd * s.view.scale;
    const float px = x0 + dx * l2, py = y0 + dy * l2;
    disc(c, px, py, 3.5f, 3.0f, kGold, 120);
    ring(c, px, py, 4.5f, 3.8f, 1.6f, kGold, 255);
  }
}

void drawPreview(uint16_t* c, const Scene& s) {
  const uint8_t alpha = s.previewAlpha ? s.previewAlpha : 230;
  const uint16_t col = s.previewColor ? s.previewColor : (s.previewWarn ? rgb(255, 190, 110) : kWhite);
  float carry = 0;                                  // walk the polyline, a dot every 7 px
  for (int i = 1; i < s.previewCount; ++i) {
    const float ax = sxOf(s, s.preview[i - 1].x), ay = syOf(s, s.preview[i - 1].y);
    const float bx = sxOf(s, s.preview[i].x), by = syOf(s, s.preview[i].y);
    const float len = sqrtf((bx - ax) * (bx - ax) + (by - ay) * (by - ay));
    float t = carry;
    for (; t < len; t += 7.0f) {
      const float px = ax + (bx - ax) * t / len, py = ay + (by - ay) * t / len;
      if (py > kScreenH + 2 || py < kTop - 2) continue;
      disc(c, px, py, 1.9f, 1.9f, rgb(20, 40, 20), (uint8_t)(alpha / 2));
      disc(c, px, py, 1.3f, 1.3f, col, alpha);
    }
    carry = t - len;
  }
  // Switch-Sports-style distance dots along the line (the same as on the meter)
  if (s.distCount > 0) {
    float walked = 0;
    int k = 0;
    for (int i = 1; i < s.previewCount && k < s.distCount; ++i) {
      const Point3& a = s.preview[i - 1];
      const Point3& b = s.preview[i];
      const float seg = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
      while (k < s.distCount && s.distYd[k] <= walked + seg) {
        const float t = seg > 0 ? (s.distYd[k] - walked) / seg : 0;
        const float px = sxOf(s, a.x + (b.x - a.x) * t), py = syOf(s, a.y + (b.y - a.y) * t);
        disc(c, px, py, 3.0f, 3.0f, rgb(30, 40, 20), alpha);
        disc(c, px, py, 2.1f, 2.1f, kGold, alpha);
        ++k;
      }
      walked += seg;
    }
  }
  const Point3& e = s.preview[s.previewCount - 1];
  const float mx = sxOf(s, e.x), my = syOf(s, e.y);
  const float d = sqrtf((e.x - s.preview[0].x) * (e.x - s.preview[0].x) + (e.y - s.preview[0].y) * (e.y - s.preview[0].y));
  const float rr = fmaxf(4.0f, d * 0.05f * s.view.scale);
  disc(c, mx, my, rr, rr * 0.8f, col, (uint8_t)(alpha / 5));
  ring(c, mx, my, rr, rr * 0.8f, 1.4f, col, alpha);
  line(c, mx - 2.5f, my, mx + 2.5f, my, 1.2f, col, alpha);
  line(c, mx, my - 2.5f, mx, my + 2.5f, 1.2f, col, alpha);
}

void drawSlope(uint16_t* c, const Scene& s) {
  if (!s.map) return;
  const float sl = sqrtf(s.slopeX * s.slopeX + s.slopeY * s.slopeY);
  if (sl < 0.005f) return;
  const float ux = s.slopeX / sl, uy = s.slopeY / sl;
  const float step = 2.0f;
  const float phase = fmodf(s.frame * 0.12f * (0.5f + sl * 8.0f), step);
  const HoleDef& d = s.map->def();
  const float R = (d.greenRx > d.greenRy ? d.greenRx : d.greenRy) + 1;
  for (float gy = -R; gy <= R; gy += step)
    for (float gx = -R; gx <= R; gx += step) {
      const float wx = d.greenX + gx + ux * phase, wy = d.greenY + gy + uy * phase;
      if (!s.map->onGreen(wx, wy)) continue;
      const float px = sxOf(s, wx), py = syOf(s, wy);
      const float fade = 0.5f + 0.5f * sinf(phase / step * 3.14159f);
      disc(c, px, py, 1.1f, 1.1f, rgb(255, 255, 255), (uint8_t)(70 + 90 * fade));
    }
  // a legend arrow in the corner
  const float cx = 254, cy = 112;
  panel(c, 234, 92, 42, 38, 150);
  const float vx = ux, vy = -uy;
  line(c, cx - vx * 10, cy - vy * 10, cx + vx * 10, cy + vy * 10, 2.4f, kWhite, 255);
  line(c, cx + vx * 10, cy + vy * 10, cx + vx * 4 - vy * 5, cy + vy * 4 + vx * 5, 2.4f, kWhite, 255);
  line(c, cx + vx * 10, cy + vy * 10, cx + vx * 4 + vy * 5, cy + vy * 4 - vx * 5, 2.4f, kWhite, 255);
  text(c, "slope", 255, 127, 11, kSoft, 1, false);
}

void drawWind(uint16_t* c, const Scene& s) {
  const float cx = 254, cy = 62;
  disc(c, cx, cy, 20, 20, rgb(16, 28, 36), 175);
  ring(c, cx, cy, 20, 20, 1.2f, kWhite, 60);
  char buf[12];
  if (s.windMph < 0.5f) {
    text(c, "calm", (int)cx, (int)cy + 4, 11, kWhite, 1, false);
    return;
  }
  const float a = s.windDeg * 0.0174533f;
  const float vx = sinf(a), vy = -cosf(a);
  const float L = 9;
  const float tx = cx + vx * L, ty = cy - 3 + vy * L;
  line(c, cx - vx * L, cy - 3 - vy * L, tx, ty, 2.6f, rgb(140, 220, 255), 255);
  line(c, tx, ty, tx - vx * 5 - vy * 4, ty - vy * 5 + vx * 4, 2.6f, rgb(140, 220, 255), 255);
  line(c, tx, ty, tx - vx * 5 + vy * 4, ty - vy * 5 - vx * 4, 2.6f, rgb(140, 220, 255), 255);
  snprintf(buf, sizeof(buf), "%d mph", (int)lroundf(s.windMph));
  text(c, buf, (int)cx, (int)cy + 15, 11, kWhite, 1, false);
}

int clubIcon(int club) { return club <= kWood3 ? 0 : (club <= kIron9 ? 1 : (club == kPutter ? 3 : 2)); }

void drawHud(uint16_t* c, const Scene& s) {
  char buf[40];
  panel(c, 4, 42, 112, 36, 185);
  if (s.hole > 0) {
    snprintf(buf, sizeof(buf), "Hole %d", s.hole);
    text(c, buf, 11, 58, 13, kWhite);
    snprintf(buf, sizeof(buf), "Par %d", s.par);
    text(c, buf, 109, 58, 13, kGold, 2);
    snprintf(buf, sizeof(buf), "%d yd  -  shot %d", s.yards, s.stroke);
  } else {
    text(c, s.title ? s.title : "Driving range", 11, 58, 13, kWhite);
    snprintf(buf, sizeof(buf), "targets 50-250 yd");
  }
  text(c, buf, 11, 72, 11, kSoft);
  drawWind(c, s);
  // bottom bar: club and distances
  const int cw = 34 + textWidth(club(s.club).name, 13);
  snprintf(buf, sizeof(buf), s.club == kPutter ? "%d yd max" : "%d yd", (int)lroundf(s.clubYd));
  const int bw = cw > textWidth(buf, 11) + 34 ? cw : textWidth(buf, 11) + 34;
  panel(c, 4, kBoxY, bw, 32, 175);
  sprite(c, golf_art::kClubIcons[clubIcon(s.club)], 7, kBoxY + 6);
  text(c, club(s.club).name, 30, kBoxY + 14, 13, kWhite);
  text(c, buf, 30, kBoxY + 27, 11, kGold, 0, false);
  snprintf(buf, sizeof(buf), "pin %d yd", (int)lroundf(s.toPin));
  const int pw = textWidth(buf, 13) + 14;
  panel(c, 276 - pw, kBoxY, pw, 32, 175);
  text(c, buf, 270, kBoxY + 14, 13, kWhite, 2);
  if (s.lieName) text(c, s.lieName, 270, kBoxY + 27, 11, kSoft, 2, false);
}

// The Shot Meter (far left) and the feedback bars: power, forward-swing speed, face, path, follow-through.
uint32_t feedbackKey(const Scene& s) {
  const SwingFeedback& f = *s.feedback;
  uint32_t h = 2166136261u;
  auto mix = [&](int32_t v) { h = (h ^ (uint32_t)v) * 16777619u; };
  // redraw only when something moves by a pixel
  mix((int)(f.fill * 62)); mix((int)(f.target * 62)); mix(f.red); mix(f.phase); mix(f.frozen);
  mix((int)lroundf(s.meterBend.bend * 12)); mix((int)lroundf(s.meterBend.overshoot * 62)); mix((int)s.meterBend.overSide);
  if (f.red && !f.frozen) mix(s.frame);                          // the live over-swing squiggle moves
  return h | 1u;                                                 // never 0 (0 = empty cache)
}

void drawFeedbackColumn(uint16_t* c, const Scene& s);

void drawFeedback(uint16_t* c, const Scene& s) {
  if (s.feedbackCache && s.feedbackKey) {
    const uint32_t key = feedbackKey(s);
    if (*s.feedbackKey == key) {                                  // unchanged: copy the cached pixels back
      for (int y = 0; y < kColH; ++y)
        memcpy(c + (kColY + y) * kScreenW + kFeedbackX, s.feedbackCache + y * kColW, (size_t)kColW * 2);
      return;
    }
    drawFeedbackColumn(c, s);
    for (int y = 0; y < kColH; ++y)
      memcpy(s.feedbackCache + y * kColW, c + (kColY + y) * kScreenW + kFeedbackX, (size_t)kColW * 2);
    *s.feedbackKey = key;
    return;
  }
  drawFeedbackColumn(c, s);
}

void drawFeedbackColumn(uint16_t* c, const Scene& s) {
  const SwingFeedback& f = *s.feedback;
  const MeterBend& mb = s.meterBend;
  // opaque (so it can be cached): a dark card with a light edge
  rect(c, kFeedbackX, kColY, kColW, kColH, rgb(18, 28, 36), 255);
  rect(c, kFeedbackX, kColY, kColW, 1, rgb(255, 255, 255), 40);
  // The bar bends like Switch Sports' Shot Meter: its centreline leans the way the ball will curve (more toward the
  // top); an over-swing overshoots the 100 % mark in red and whips to the side the ball drifts.
  const float cx0 = kFeedbackX + kColW / 2.0f;
  const int y1 = kColY + kColH - 8;                              // the bottom of the bar
  const float h100 = 62.0f;                                      // 100 % is this many px up
  const float bendPx = mb.bend * 12.0f;
  auto centre = [&](float t) {                                   // t: share of 100 % (overshoot above 1)
    float x = cx0 + bendPx * fminf(t, 1.0f) * fminf(t, 1.0f);
    if (t > 1.0f && mb.overshoot > 0) {
      const float u = fminf(1.0f, (t - 1.0f) / mb.overshoot);
      x += mb.overSide * 11.0f * u * u;                          // the whip (fits the panel either way)
    }
    const float lim = kFeedbackX + 6.0f, rim = kFeedbackX + kColW - 6.0f;
    return x < lim ? lim : (x > rim ? rim : x);
  };
  const bool red = f.red;
  const bool weak = f.phase >= 2 && !red && f.fill < f.target * 0.9f;
  const uint16_t fillCol = red ? rgb(240, 60, 50) : (f.phase <= 1 ? rgb(70, 150, 255) : (weak ? rgb(255, 170, 60) : rgb(110, 215, 90)));
  const float topT = 1.0f + (mb.overshoot > 0 ? mb.overshoot : 0.0f);
  const float fillT = red ? topT : fmaxf(0.0f, fminf(f.fill, 1.0f));
  const float half = 4.5f;
  for (int y = y1; y >= y1 - (int)ceilf(topT * h100); --y) {
    const float t = (y1 - y) / h100;
    if (t > topT) break;
    const float x = centre(t);
    const uint16_t col = t <= fillT ? fillCol : rgb(8, 12, 16);
    // an anti-aliased run around the centreline
    const float l = x - half, r = x + half;
    const int il = (int)ceilf(l), ir = (int)floorf(r);
    hspan(c, y, il, ir, col, 255);
    blend(c, il - 1, y, col, (uint8_t)(255 * (il - l)));
    blend(c, ir, y, col, (uint8_t)(255 * (r - ir)));
  }
  // plain dots at 25 / 50 / 75 / 100 % (the same as on the course line)
  for (int k = 1; k <= 4; ++k) {
    const float t = k * 0.25f;
    disc(c, centre(t), y1 - t * h100, 1.8f, 1.8f, t <= fillT ? kWhite : kGold, 255);
  }
  // the bold 100 % mark, across the bar where it is
  const float x100 = centre(1.0f), yy = y1 - h100;
  rect(c, (int)lroundf(x100 - 8), (int)lroundf(yy - 1), 17, 3, kWhite, 255);
  // the target the backswing set: an arrow on the left of the bar
  if (f.target > 0.02f) {
    const float tt = fminf(f.target, 1.0f);
    const float tx = centre(tt) - half - 2, ty = y1 - tt * h100;
    for (int k = 0; k < 5; ++k) {
      const int w = k < 3 ? k + 2 : 5 - k + 1;
      hspan(c, (int)lroundf(ty) - 2 + k, (int)lroundf(tx) - w, (int)lroundf(tx), kGold, 255);
    }
  }
}

void drawBanner(uint16_t* c, const Scene& s) {
  const int w1 = textWidth(s.banner, 25), w2 = s.sub ? textWidth(s.sub, 13) : 0;
  const int w = (w1 > w2 ? w1 : w2) + 28;
  const int h = s.sub ? 52 : 36;
  const int cx = s.feedback ? 162 : kScreenW / 2;               // beside the meter when it is up
  const int x = cx - w / 2, y = 94;
  panel(c, x, y, w, h, 205);
  text(c, s.banner, cx, y + 28, 25, s.bannerColor ? s.bannerColor : kWhite, 1);
  if (s.sub) text(c, s.sub, cx, y + 45, 13, kSoft, 1);
}

void drawLines(uint16_t* c, const Scene& s) {
  const int size = s.feedback ? 11 : 13;                        // beside the feedback column: the small font
  const int lh = s.feedback ? 13 : 15;
  int n = 0, w = 0;
  for (int i = 0; i < 4; ++i)
    if (s.lines[i]) { ++n; const int lw = textWidth(s.lines[i], size); if (lw > w) w = lw; }
  if (!n) return;
  const int cx = s.feedback ? 162 : kScreenW / 2;
  const int y = s.banner ? 152 : 100, h = n * lh + 10, x = cx - (w + 16) / 2;
  panel(c, x, y, w + 16, h, 195);
  for (int i = 0, k = 0; i < 4; ++i)
    if (s.lines[i]) text(c, s.lines[i], cx, y + lh + 3 + lh * k++, size, i == 0 ? kGold : kWhite, 1, false);
}

// The bottom strip: every hint and status line, never over the panels (primary on the left, help on the right).
void drawStrip(uint16_t* c, const char* hint, bool help) {
  if (!hint && !help) return;
  rect(c, 0, kStripY, kScreenW, kScreenH - kStripY, rgb(10, 18, 24), 190);
  const char* h = "hold A: help";
  const int hw = help ? textWidth(h, 11) : 0;
  if (hint) {
    const int w = textWidth(hint, 11);
    if (!help || w + hw + 20 <= kScreenW - 8) text(c, hint, help ? 6 : kScreenW / 2, kStripY + 12, 11, kSoft, help ? 0 : 1, false);
    else text(c, hint, 6, kStripY + 12, 11, kSoft, 0, false);   // long: the help label gives way
    if (help && w + hw + 20 <= kScreenW - 8) text(c, h, kScreenW - 6, kStripY + 12, 11, rgb(150, 165, 175), 2, false);
  } else {
    text(c, "hold A: how to play", kScreenW / 2, kStripY + 12, 11, kSoft, 1, false);
  }
}
void drawHint(uint16_t* c, const char* hint) { drawStrip(c, hint, false); }

void drawMenu(uint16_t* c, const Scene& s) {
  // title block
  panel(c, 8, 44, 128, 62, 175);
  text(c, "Golf", 72, 80, 25, kWhite, 1);
  if (s.title) text(c, s.title, 72, 99, 11, kGold, 1, false);
  const int x = 146, w = 128, rowH = 24, maxRows = 6;
  const int rows = s.itemCount < maxRows ? s.itemCount : maxRows;
  int first = s.selected - 2;
  if (first > s.itemCount - rows) first = s.itemCount - rows;
  if (first < 0) first = 0;
  const int h = rows * rowH + 10;
  const int y = 44;
  panel(c, x, y, w, h, 185);
  if (first > 0) text(c, "^", x + w - 12, y + 14, 11, kSoft, 0, false);
  if (first + rows < s.itemCount) text(c, "v", x + w - 12, y + h - 4, 11, kSoft, 0, false);
  for (int i = first; i < first + rows; ++i) {
    const int ry = y + 5 + (i - first) * rowH;
    if (i == s.selected) {
      rect(c, x + 4, ry, w - 8, rowH - 2, rgb(70, 160, 80), 230);
      rect(c, x + 4, ry, 3, rowH - 2, kGold, 255);
    }
    text(c, s.items[i], x + 12, ry + 16, 13, i == s.selected ? kWhite : kSoft, 0, i == s.selected);
  }
}

void drawIntro(uint16_t* c, const Scene& s) {
  char buf[40];
  panel(c, 6, 44, 132, 150, 190);
  if (s.courseName) text(c, s.courseName, 72, 62, 11, kGold, 1, false);
  if (s.hole > 0) {
    snprintf(buf, sizeof(buf), "Hole %d", s.hole);
    text(c, buf, 72, 92, 25, kWhite, 1);
    snprintf(buf, sizeof(buf), "Par %d", s.par);
    text(c, buf, 72, 116, 13, kWhite, 1);
  } else {
    text(c, "Range", 72, 92, 25, kWhite, 1);
    text(c, "practice", 72, 116, 13, kWhite, 1);
  }
  snprintf(buf, sizeof(buf), "%d yards", s.yards);
  text(c, buf, 72, 134, 13, kSoft, 1);
  if (s.windMph >= 0.5f) {
    const float a = s.windDeg * 0.0174533f, vx = sinf(a), vy = -cosf(a);
    const float cx = 40, cy = 160;
    line(c, cx - vx * 8, cy - vy * 8, cx + vx * 8, cy + vy * 8, 2.4f, rgb(140, 220, 255), 255);
    line(c, cx + vx * 8, cy + vy * 8, cx + vx * 3 - vy * 4, cy + vy * 3 + vx * 4, 2.4f, rgb(140, 220, 255), 255);
    line(c, cx + vx * 8, cy + vy * 8, cx + vx * 3 + vy * 4, cy + vy * 3 - vx * 4, 2.4f, rgb(140, 220, 255), 255);
    snprintf(buf, sizeof(buf), "wind %d mph", (int)lroundf(s.windMph));
    text(c, buf, 56, 165, 11, kWhite, 0, false);
  } else {
    text(c, "no wind", 72, 165, 11, kWhite, 1, false);
  }
  if (s.progress < 100) {
    rect(c, 18, 178, 108, 6, rgb(10, 14, 18), 220);
    rect(c, 18, 178, 108 * s.progress / 100, 6, rgb(90, 200, 90), 255);
  } else {
    text(c, s.stroke > 1 ? "press C" : "press C to play", 72, 186, 11, kGold, 1, false);
  }
}

void drawCard(uint16_t* c, const Scene& s) {
  char buf[16];
  const int x = 8, y = 48, w = 264, cellW = 21, labelW = 40;
  panel(c, x, y, w, 132, 215);
  if (s.title) text(c, s.title, kScreenW / 2, y + 20, 13, kWhite, 1);
  const int ry[3] = {y + 44, y + 66, y + 90};
  text(c, "Hole", x + 8, ry[0], 11, kSoft, 0, false);
  text(c, "Par", x + 8, ry[1], 11, kSoft, 0, false);
  text(c, "You", x + 8, ry[2], 11, kWhite, 0, false);
  int parSum = 0, sum = 0, playedPar = 0;
  for (int i = 0; i < s.holeCount && i < 9; ++i) {
    const int cx = x + labelW + i * cellW + cellW / 2;
    snprintf(buf, sizeof(buf), "%d", i + 1);
    text(c, buf, cx, ry[0], 11, kSoft, 1, false);
    snprintf(buf, sizeof(buf), "%d", s.pars[i]);
    text(c, buf, cx, ry[1], 13, kWhite, 1, false);
    parSum += s.pars[i];
    if (s.strokes && s.strokes[i]) {
      const int d = s.strokes[i] - s.pars[i];
      if (d < 0) ring(c, cx - 0.5f, ry[2] - 5, 8, 8, 1.4f, rgb(240, 90, 80), 255);
      if (d > 0) rect(c, cx - 8, ry[2] - 13, 16, 16, rgb(90, 140, 220), 110);
      snprintf(buf, sizeof(buf), "%d", s.strokes[i]);
      text(c, buf, cx, ry[2], 13, d < 0 ? rgb(255, 150, 140) : kWhite, 1, false);
      sum += s.strokes[i];
      playedPar += s.pars[i];
    }
    rect(c, cx + cellW / 2, y + 30, 1, 68, kWhite, 40);
  }
  rect(c, x + 6, ry[0] + 5, w - 12, 1, kWhite, 60);
  snprintf(buf, sizeof(buf), "%d", parSum);
  text(c, "Out", x + w - 8, ry[0], 11, kSoft, 2, false);
  text(c, buf, x + w - 8, ry[1], 13, kWhite, 2, false);
  snprintf(buf, sizeof(buf), "%d", sum);
  text(c, buf, x + w - 8, ry[2], 13, kGold, 2, false);
  if (sum) {
    const int d = sum - playedPar;
    if (d == 0) snprintf(buf, sizeof(buf), "Even");
    else snprintf(buf, sizeof(buf), "%+d", d);
    text(c, buf, kScreenW / 2, y + 122, 13, d < 0 ? rgb(255, 150, 140) : (d == 0 ? kWhite : kGold), 1);
  }
}

// The "how to play" card: translucent and rounded like the Wii's, a page per topic, dots for the pages.
void drawHelp(uint16_t* c, const Scene& s) {
  panel(c, 6, 42, 268, 194, 228);
  text(c, "How to play: Golf", kScreenW / 2, 60, 13, kGold, 1);
  const HelpPage& p = helpPage(s.helpPage);
  text(c, p.title, 18, 82, 13, kWhite);
  rect(c, 18, 87, 244, 1, kWhite, 70);
  for (int i = 0; i < 7 && p.lines[i]; ++i) text(c, p.lines[i], 18, 104 + i * 15, 11, kSoft);
  const int n = helpPageCount();
  for (int i = 0; i < n; ++i) {
    const float x = kScreenW / 2 + (i - (n - 1) / 2.0f) * 11;
    disc(c, x, 212, i == s.helpPage ? 3.0f : 2.2f, i == s.helpPage ? 3.0f : 2.2f,
         i == s.helpPage ? kGold : rgb(120, 130, 140), 255);
  }
  text(c, "dial: pages    tap A, B or C: close", kScreenW / 2, 230, 11, kSoft, 1);
}


}  // namespace

int followCam(const View& v, int imageH, float ballY, float aheadY) {
  const int visible = kScreenH - kTop;
  const float by = viewY(v, ballY), ay = viewY(v, aheadY);
  float cam = by - (visible - 34);                 // ball 34 px above the bottom edge
  if (ay - 48 < cam) cam = ay - 48;                // the target wins: the ball may go below the screen
  if (cam > imageH - visible) cam = (float)(imageH - visible);
  if (cam < 0) cam = 0;
  return (int)cam;
}

void clearScene(Scene& s) {
  memset(&s, 0, sizeof(s));
  s.markerYd = -1;
  s.progress = 100;
  s.golferFrame = 0;
}

void drawScene(uint16_t* c, const Scene& s, DrawTiming* tm) {
  uint32_t t0 = tm && tm->clock ? tm->clock() : 0, tBg = t0, tWorld = t0;
  auto stamp = [&](uint32_t* at) { if (tm && tm->clock) *at = tm->clock(); };
  struct Finish {
    DrawTiming* tm;
    const uint32_t* t0;
    const uint32_t* tBg;
    const uint32_t* tWorld;
    ~Finish() {
      if (!tm || !tm->clock) return;
      const uint32_t end = tm->clock();
      tm->bgUs = *tBg - *t0;
      tm->worldUs = *tWorld - *tBg;
      tm->hudUs = end - *tWorld;
    }
  } finish = {tm, &t0, &tBg, &tWorld};
  if (s.screen == Screen::Intro) {
    // overview on the right, card on the left
    const uint16_t bg = (uint16_t)((rgb(18, 36, 24) >> 8) | (rgb(18, 36, 24) << 8));
    const uint32_t bg2 = bg | ((uint32_t)bg << 16);
    uint32_t* c32 = (uint32_t*)c;
    for (int i = 0; i < kScreenW * kScreenH / 2; ++i) c32[i] = bg2;
    if (s.image && s.progress >= 100) {
      const int ox = kScreenW - s.imageW - 6 < 144 ? 144 : kScreenW - s.imageW - 6;
      const int oy = kTop + (kScreenH - kTop - s.imageH) / 2;
      for (int y = 0; y < s.imageH; ++y) {
        const int sy = oy + y;
        if (sy < kTop || sy >= kScreenH) continue;
        const int n = s.imageW < kScreenW - ox ? s.imageW : kScreenW - ox;
        memcpy(c + sy * kScreenW + ox, s.image + y * s.imageW, (size_t)n * 2);
      }
      // flag and tee on the overview
      Scene o = s;
      o.camX = -ox;
      o.camY = kTop - oy;
      drawFlagAt(c, o, s.pinX, s.pinY, false);
      if (s.showBall) drawBall(c, o);
    }
    stamp(&tBg);
    stamp(&tWorld);
    drawIntro(c, s);
    if (!s.helpOpen) drawStrip(c, s.hint, s.helpHint);
    if (s.helpOpen) drawHelp(c, s);
    return;
  }
  drawImage(c, s);
  stamp(&tBg);
  tWorld = tBg;
  switch (s.screen) {
    case Screen::Title:
    case Screen::CourseMenu:
      drawMenu(c, s);
      if (!s.helpOpen) drawStrip(c, s.hint, s.helpHint);
      if (s.helpOpen) drawHelp(c, s);
      return;
    case Screen::Calibrate:
      if (s.banner) drawBanner(c, s);
      if (s.hint) drawHint(c, s.hint);
      return;
    case Screen::Card:
      drawCard(c, s);
      if (!s.helpOpen) drawStrip(c, s.hint, s.helpHint);
      if (s.helpOpen) drawHelp(c, s);
      return;
    default:
      break;
  }
  // play
  if (s.showSlope) drawSlope(c, s);
  if (s.rangeTargets && s.map) {
    for (int i = 0; i < s.map->shapeCount(); ++i) {
      const Shape& sh = s.map->shape(i);
      if (sh.surface != Surface::Green) continue;
      drawFlagAt(c, s, sh.ax, sh.ay, false);
    }
  }
  const bool ballBehindFlag = s.ballY > s.pinY;
  if (s.showBall && ballBehindFlag) drawBall(c, s);
  drawFlagAt(c, s, s.pinX, s.pinY, s.bigFlag);
  if (s.preview && s.previewCount > 1) drawPreview(c, s);
  else if (s.showAim) drawAim(c, s);
  if (s.showBall && !ballBehindFlag) drawBall(c, s);
  if (s.rangeTargets && s.map) {
    char buf[8];
    for (int i = 0; i < s.map->shapeCount(); ++i) {
      const Shape& sh = s.map->shape(i);
      if (sh.surface != Surface::Green) continue;
      snprintf(buf, sizeof(buf), "%d", sh.ay);
      text(c, buf, (int)sxOf(s, sh.ax) + (int)(sh.ra * s.view.scale) + 3, (int)syOf(s, sh.ay) + 4, 11, kWhite, 0, true);
    }
    snprintf(buf, sizeof(buf), "%d", (int)s.pinY);
    text(c, buf, (int)sxOf(s, s.pinX) + 10, (int)syOf(s, s.pinY) + 4, 11, kWhite, 0, true);
  }
  stamp(&tWorld);
  if (s.hud) drawHud(c, s);
  if (s.feedback && s.feedback->active) drawFeedback(c, s);
  if (s.banner) drawBanner(c, s);
  drawLines(c, s);
  if (!s.helpOpen) drawStrip(c, s.hint, s.helpHint);
  if (s.helpOpen) drawHelp(c, s);
}

}  // namespace golf
}  // namespace ui
