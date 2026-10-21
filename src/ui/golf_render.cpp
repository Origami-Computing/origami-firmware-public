// Golf hole renderer: see include/ui/golf_render.h.
#include "ui/golf_render.h"

#include <math.h>
#include <string.h>

#include "assets/golf_art.h"

namespace ui {
namespace golf {
using namespace core::golf;

namespace {

struct Paint {
  uint8_t detail;
  uint8_t a[3], b[3];
};
struct ThemePaint {
  Paint ob, rough, fairway, fringe, green, tee, bunker, water, waste, canyon;
  uint8_t marker[3];
};
using namespace golf_art;
#define P(d, a0, a1, a2, b0, b1, b2) {d, {a0, a1, a2}, {b0, b1, b2}}
const ThemePaint kPaints[4] = {
    // Parkland
    {P(kDWoods, 0x2c, 0x54, 0x24, 0x3f, 0x64, 0x28), P(kDRough, 0x3d, 0x8a, 0x30, 0x56, 0x99, 0x3a),
     P(kDFine, 0x5d, 0xb5, 0x4a, 0x72, 0xc2, 0x58), P(kDFine, 0x4f, 0xa5, 0x40, 0x62, 0xb0, 0x4a),
     P(kDFine, 0x6b, 0xc9, 0x55, 0x7f, 0xd4, 0x66), P(kDFine, 0x63, 0xbb, 0x4e, 0x77, 0xc8, 0x5c),
     P(kDSand, 0xef, 0xe2, 0xb8, 0xe0, 0xcf, 0x9c), P(kDWater, 0x3a, 0x9a, 0xd0, 0x1d, 0x5f, 0x9c),
     P(kDPebbles, 0xd8, 0xc9, 0xa0, 0xc8, 0xb8, 0x8c), P(kDRock, 0xb8, 0x58, 0x3c, 0xd6, 0x80, 0x4f), {60, 110, 230}},
    // Links
    {P(kDFescue, 0x6c, 0x7a, 0x3c, 0x8e, 0x88, 0x50), P(kDFescue, 0x8f, 0x9e, 0x4c, 0xc7, 0xb5, 0x6c),
     P(kDFine, 0x8c, 0xbc, 0x5a, 0xa2, 0xc9, 0x6c), P(kDFine, 0x7f, 0xae, 0x50, 0x90, 0xb9, 0x5c),
     P(kDFine, 0x7c, 0xc6, 0x5c, 0x8f, 0xd0, 0x6c), P(kDFine, 0x8c, 0xbc, 0x5a, 0x9c, 0xc8, 0x68),
     P(kDSand, 0xd6, 0xc0, 0x8a, 0xc4, 0xaa, 0x74), P(kDWater, 0x2e, 0x86, 0xb8, 0x17, 0x50, 0x7e),
     P(kDSand, 0xea, 0xdb, 0xb0, 0xd9, 0xc5, 0x95), P(kDRock, 0xb8, 0x58, 0x3c, 0xd6, 0x80, 0x4f), {235, 235, 235}},
    // Desert
    {P(kDPebbles, 0xc0, 0x84, 0x50, 0xa3, 0x6a, 0x40), P(kDRough, 0x8f, 0x91, 0x50, 0xa8, 0xa0, 0x60),
     P(kDFine, 0x5f, 0xb2, 0x4a, 0x74, 0xc0, 0x5a), P(kDFine, 0x58, 0xa4, 0x44, 0x69, 0xb1, 0x50),
     P(kDFine, 0x6b, 0xc9, 0x55, 0x7f, 0xd4, 0x66), P(kDFine, 0x63, 0xbb, 0x4e, 0x77, 0xc8, 0x5c),
     P(kDSand, 0xf4, 0xee, 0xdc, 0xe8, 0xdf, 0xc4), P(kDWater, 0x35, 0xb4, 0xc4, 0x1a, 0x7a, 0x94),
     P(kDPebbles, 0xe2, 0xa8, 0x6c, 0xcc, 0x8e, 0x58), P(kDRock, 0xb8, 0x58, 0x3c, 0xd6, 0x80, 0x4f), {220, 60, 50}},
    // Range (parkland colours, a manicured practice ground)
    {P(kDWoods, 0x2c, 0x54, 0x24, 0x3f, 0x64, 0x28), P(kDRough, 0x3d, 0x8a, 0x30, 0x56, 0x99, 0x3a),
     P(kDFine, 0x5d, 0xb5, 0x4a, 0x72, 0xc2, 0x58), P(kDFine, 0x4f, 0xa5, 0x40, 0x62, 0xb0, 0x4a),
     P(kDFine, 0x6b, 0xc9, 0x55, 0x7f, 0xd4, 0x66), P(kDFine, 0x63, 0xbb, 0x4e, 0x77, 0xc8, 0x5c),
     P(kDSand, 0xef, 0xe2, 0xb8, 0xe0, 0xcf, 0x9c), P(kDWater, 0x3a, 0x9a, 0xd0, 0x1d, 0x5f, 0x9c),
     P(kDPebbles, 0xd8, 0xc9, 0xa0, 0xc8, 0xb8, 0x8c), P(kDRock, 0xb8, 0x58, 0x3c, 0xd6, 0x80, 0x4f), {230, 200, 40}},
};
#undef P

struct Rgb {
  float r, g, b;
};
inline Rgb lerp(const Rgb& a, const Rgb& b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }
inline Rgb mul(const Rgb& a, float k) { return {a.r * k, a.g * k, a.b * k}; }
inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

inline Rgb texel(const Paint& p, int px, int py, float* lumOut = nullptr) {
  const uint8_t* t = kDetail[p.detail] + ((((py & (kDetailSize - 1)) * kDetailSize) + (px & (kDetailSize - 1))) << 1);
  const float L = t[0] * (1.0f / 128.0f), m = t[1] * (1.0f / 255.0f);
  if (lumOut) *lumOut = L;
  return {(p.a[0] + (p.b[0] - p.a[0]) * m) * L, (p.a[1] + (p.b[1] - p.a[1]) * m) * L, (p.a[2] + (p.b[2] - p.a[2]) * m) * L};
}

inline uint16_t pack(const Rgb& c) {
  const int r = c.r < 0 ? 0 : (c.r > 255 ? 255 : (int)c.r);
  const int g = c.g < 0 ? 0 : (c.g > 255 ? 255 : (int)c.g);
  const int b = c.b < 0 ? 0 : (c.b > 255 ? 255 : (int)c.b);
  const uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  return (uint16_t)((v >> 8) | (v << 8));
}
inline Rgb unpack(uint16_t sw) {
  const uint16_t v = (uint16_t)((sw >> 8) | (sw << 8));
  return {(float)(((v >> 11) & 31) * 255 / 31), (float)(((v >> 5) & 63) * 255 / 63), (float)((v & 31) * 255 / 31)};
}

bool isHazard(Surface s) { return s == Surface::Bunker || s == Surface::Water || s == Surface::Canyon; }

const Paint& paintFor(const ThemePaint& tp, Surface s) {
  switch (s) {
    case Surface::Rough: return tp.rough;
    case Surface::Fairway: return tp.fairway;
    case Surface::Green: return tp.green;
    case Surface::Tee: return tp.tee;
    case Surface::Bunker: return tp.bunker;
    case Surface::Water: return tp.water;
    case Surface::Waste: return tp.waste;
    case Surface::Canyon: return tp.canyon;
    case Surface::Fringe: return tp.fringe;
    default: return tp.ob;
  }
}

constexpr float kLx = -0.6f, kLy = 0.8f;       // toward the sun (top left), world frame

}  // namespace

View holeView(const HoleMap& map, int w, int maxH, float maxScale) {
  const float wy = map.maxY() - map.minY(), wx = map.maxX() - map.minX();
  float s = maxScale;
  if (maxH / wy < s) s = maxH / wy;
  if (w / wx < s) s = w / wx;
  View v;
  v.scale = s;
  v.w = w;
  v.h = (int)ceilf(wy * s);
  if (v.h > maxH) v.h = maxH;
  v.x0 = (map.minX() + map.maxX()) * 0.5f - w * 0.5f / s;
  v.y0 = map.maxY();
  return v;
}

View greenView(const HoleMap& map, int w, int h) {
  const HoleDef& d = map.def();
  const float r = (d.greenRx > d.greenRy ? d.greenRx : d.greenRy) + kFringe;
  float s = w / (2.0f * (r + 9.0f));
  if (h / (2.0f * (r + 5.0f)) < s) s = h / (2.0f * (r + 5.0f));
  View v;
  v.scale = s;
  v.w = w;
  v.h = h;
  v.x0 = d.greenX - w * 0.5f / s;
  v.y0 = d.greenY + h * 0.5f / s;
  return v;
}

void HoleRenderer::begin(const HoleMap* map, const View& view, uint16_t* dst, int stride, bool greenGrid) {
  map_ = map;
  v_ = view;
  dst_ = dst;
  stride_ = stride;
  grid_ = greenGrid;
  row_ = 0;
  phase_ = 0;
  const float s = view.scale;
  auto bounds = [&](const Shape& sh, float extra, int16_t* t, int16_t* b, int16_t* l, int16_t* r) {
    float x0, x1, y0, y1;
    if (sh.kind == ShapeKind::Capsule) {
      const float rr = (sh.ra > sh.rb ? sh.ra : sh.rb) + extra;
      x0 = fminf(sh.ax, sh.bx) - rr; x1 = fmaxf(sh.ax, sh.bx) + rr;
      y0 = fminf(sh.ay, sh.by) - rr; y1 = fmaxf(sh.ay, sh.by) + rr;
    } else {
      const float rr = (sh.ra > sh.rb ? sh.ra : sh.rb) + extra;
      x0 = sh.ax - rr; x1 = sh.ax + rr; y0 = sh.ay - rr; y1 = sh.ay + rr;
    }
    const float px0 = viewX(v_, x0) - 2, px1 = viewX(v_, x1) + 2, py0 = viewY(v_, y1) - 2, py1 = viewY(v_, y0) + 2;
    *l = (int16_t)fmaxf(-1, fminf(30000, px0));
    *r = (int16_t)fmaxf(-1, fminf(30000, px1));
    *t = (int16_t)fmaxf(-1, fminf(30000, py0));
    *b = (int16_t)fmaxf(-1, fminf(30000, py1));
  };
  for (int i = 0; i < map->shapeCount() && i < HoleMap::kMaxShapes; ++i) {
    const Shape& sh = map->shape(i);
    const float extra = HoleMap::edgeAmplitude(sh.surface) + 1.5f + 1.0f / s;
    bounds(sh, extra, &top_[i], &bottom_[i], &left_[i], &right_[i]);
  }
  const HoleDef& d = map->def();
  Shape g = {ShapeKind::Ellipse, Surface::Green, d.greenX, d.greenY, 0, 0, d.greenRx, d.greenRy};
  bounds(g, kFringe + 2.0f + 1.0f / s, &gTop_, &gBottom_, &gLeft_, &gRight_);
  bounds(map->teeShape(), 1.0f + 1.0f / s, &tTop_, &tBottom_, &tLeft_, &tRight_);
}

int HoleRenderer::percent() const {
  if (phase_ == 2) return 100;
  return v_.h > 0 ? row_ * 95 / v_.h : 0;
}

bool HoleRenderer::step(int rows) {
  if (phase_ == 0) {
    const int end = row_ + rows < v_.h ? row_ + rows : v_.h;
    for (; row_ < end; ++row_) renderRow(row_);
    if (row_ >= v_.h) phase_ = 1;
    return false;
  }
  if (phase_ == 1) {
    renderObjects();
    phase_ = 2;
  }
  return true;
}

namespace {
enum LayerKind : uint8_t { kLOb, kLShape, kLFair, kLFringe, kLHaz, kLTee, kLGreen };
struct Layer {
  uint8_t kind, idx;
  float d, cov;
};
}  // namespace

void HoleRenderer::renderRow(int py) {
  const HoleMap& m = *map_;
  const ThemePaint& tp = kPaints[(int)m.theme()];
  const float s = v_.scale, inv = 1.0f / s;
  const float wy = v_.y0 - (py + 0.5f) * inv;
  const HoleDef& def = m.def();
  const int n = m.shapeCount() < HoleMap::kMaxShapes ? m.shapeCount() : HoleMap::kMaxShapes;
  // shapes touching this row, by drawing pass: rough/waste, fairways, range greens, hazards
  uint8_t base[HoleMap::kMaxShapes], fair[HoleMap::kMaxShapes], grn[HoleMap::kMaxShapes], haz[HoleMap::kMaxShapes];
  int nb = 0, nf = 0, ng = 0, nh = 0;
  for (int i = 0; i < n; ++i) {
    if (py < top_[i] || py > bottom_[i]) continue;
    const Surface su = m.shape(i).surface;
    if (isHazard(su)) haz[nh++] = (uint8_t)i;
    else if (su == Surface::Fairway) fair[nf++] = (uint8_t)i;
    else if (su == Surface::Green) grn[ng++] = (uint8_t)i;
    else base[nb++] = (uint8_t)i;
  }
  const bool greenRow = py >= gTop_ && py <= gBottom_;
  const bool teeRow = py >= tTop_ && py <= tBottom_;
  const Shape tee = m.teeShape();
  const float gr = def.greenRx < def.greenRy ? def.greenRx : def.greenRy;
  uint16_t* out = dst_ + py * stride_;
  const int stripe = ((int)floorf((wy + 4000.0f) / 9.0f)) & 1;
  // gentle undulation light, sampled every 4 px and interpolated
  float light[kLightMax];
  const int nl = v_.w / 4 + 2 < kLightMax ? v_.w / 4 + 2 : kLightMax;
  for (int k = 0; k < nl; ++k) {
    const float wx = v_.x0 + (k * 4 + 0.5f) * inv;
    light[k] = 1.0f + 0.065f * noise2(wx * 0.045f, wy * 0.045f, 777u) + 0.03f * noise2(wx * 0.16f, wy * 0.16f, 99u);
  }
  Layer lay[2 * HoleMap::kMaxShapes + 6];
  for (int px = 0; px < v_.w; ++px) {
    const float wx = v_.x0 + (px + 0.5f) * inv;
    int nlay = 0, top = 0;
    auto push = [&](uint8_t kind, uint8_t idx, float d, float cov) {
      if (cov >= 1.0f) top = nlay;                          // everything below is hidden
      lay[nlay++] = Layer{kind, idx, d, cov};
    };
    push(kLOb, 0, -1, 1);
    for (int k = 0; k < nb; ++k) {
      const int i = base[k];
      if (px < left_[i] || px > right_[i]) continue;
      const float d = m.edgeDistance(i, wx, wy);
      const float cov = clamp01(0.5f - d * s);
      if (cov > 0) push(kLShape, (uint8_t)i, d, cov);
    }
    float dF = 1e9f;
    for (int k = 0; k < nf; ++k) {
      const int i = fair[k];
      if (px < left_[i] || px > right_[i]) continue;
      const float d = m.edgeDistance(i, wx, wy);
      if (d < dF) dF = d;
    }
    if (dF < 1e8f && 0.5f - dF * s > 0) push(kLFair, 0, dF, clamp01(0.5f - dF * s));
    for (int k = 0; k < ng; ++k) {
      const int i = grn[k];
      if (px < left_[i] || px > right_[i]) continue;
      const float d = m.edgeDistance(i, wx, wy);
      const float cov = clamp01(0.5f - d * s);
      if (cov > 0) push(kLShape, (uint8_t)i, d, cov);
    }
    float gd = 1e9f;
    if (greenRow && px >= gLeft_ && px <= gRight_) {
      gd = m.greenDistance(wx, wy);
      const float cov = clamp01(0.5f - (gd - kFringe) * s);
      if (cov > 0) push(kLFringe, 0, gd, cov);
    }
    for (int k = 0; k < nh; ++k) {
      const int i = haz[k];
      if (px < left_[i] || px > right_[i]) continue;
      const float d = m.edgeDistance(i, wx, wy);
      const Surface su = m.shape(i).surface;
      const float bank = su == Surface::Water ? 0.9f : (su == Surface::Bunker ? 0.55f : 1.4f);
      const float cov = clamp01(0.5f - d * s);
      if (cov > 0 || d < bank) push(kLHaz, (uint8_t)i, d, cov);
    }
    if (teeRow && px >= tLeft_ && px <= tRight_) {
      const float d = shapeDistance(tee, wx, wy);
      const float cov = clamp01(0.5f - d * s);
      if (cov > 0) push(kLTee, 0, d, cov);
    }
    if (gd < 1.0f + inv) {
      const float cov = clamp01(0.5f - gd * s);
      if (cov > 0) push(kLGreen, 0, gd, cov);
    }
    // composite from the topmost layer that covers the pixel
    Rgb col = {0, 0, 0};
    for (int li = top; li < nlay; ++li) {
      const Layer& L = lay[li];
      const float d = L.d;
      Rgb c;
      switch (L.kind) {
        case kLOb:
          c = mul(texel(tp.ob, px, py), 0.94f);
          break;
        case kLShape: {
          const Surface su = m.shape(L.idx).surface;
          c = texel(paintFor(tp, su), px, py);
          if (su == Surface::Green) {                          // range target greens
            c = mul(c, ((((int)floorf(wx / 2.5f)) + ((int)floorf(wy / 2.5f))) & 1) ? 1.04f : 0.97f);
          } else if (su == Surface::Rough && m.theme() != Theme::Desert) {
            c = mul(c, 1.0f + 0.05f * clamp01(-d / 25.0f));    // lighter toward the middle of the hole
          }
          break;
        }
        case kLFair:
          c = mul(texel(tp.fairway, px, py), stripe ? 1.06f : 0.95f);
          if (-d < 1.1f) c = mul(c, 0.9f);                      // the cut edge
          break;
        case kLFringe:
          c = texel(tp.fringe, px, py);
          break;
        case kLHaz: {
          const Surface su = m.shape(L.idx).surface;
          const float bank = su == Surface::Water ? 0.9f : (su == Surface::Bunker ? 0.55f : 1.4f);
          if (d > 0 && d < bank) {
            const float k2 = d / bank;
            col = su == Surface::Canyon ? lerp(Rgb{236, 176, 120}, col, 0.35f + 0.65f * k2) : mul(col, 0.72f + 0.28f * k2);
          }
          if (L.cov <= 0) continue;
          const float dd = -d;
          if (su == Surface::Water) {
            float Lm;
            const Paint& p = tp.water;
            texel(p, px, py, &Lm);
            const float t = clamp01(dd / 6.0f) * 0.55f + clamp01(dd / 30.0f) * 0.45f;
            c = lerp(Rgb{(float)p.a[0], (float)p.a[1], (float)p.a[2]}, Rgb{(float)p.b[0], (float)p.b[1], (float)p.b[2]}, t);
            const float hi = Lm - 1.1f;
            if (hi > 0) c = lerp(c, Rgb{225, 242, 255}, clamp01(hi * 1.6f) * (0.25f + 0.25f * t));
            if (dd < 0.6f) c = lerp(c, Rgb{190, 228, 236}, (1.0f - dd / 0.6f) * 0.55f);
          } else {
            const float e = 0.6f * inv > 0.25f ? 0.6f * inv : 0.25f;
            float gx = 0, gy = 0;
            const bool needGrad = su == Surface::Canyon ? dd < 14.0f : dd < 1.6f;
            if (needGrad) {
              gx = (m.edgeDistance(L.idx, wx + e, wy) - d) / e;
              gy = (m.edgeDistance(L.idx, wx, wy + e) - d) / e;
              const float gl = sqrtf(gx * gx + gy * gy);
              if (gl > 1e-4f) { gx /= gl; gy /= gl; }
            }
            const float facing = gx * kLx + gy * kLy;           // > 0: this wall faces away from the sun
            if (su == Surface::Bunker) {
              c = texel(tp.bunker, px, py);
              if (dd < 1.6f) {
                const float w = 1.0f - dd / 1.6f;
                c = mul(c, facing > 0 ? 1.0f - 0.42f * facing * w : 1.0f + 0.10f * (-facing) * w);
              }
              if (dd < 0.28f) c = mul(c, 0.8f);
            } else {                                              // canyon
              c = texel(tp.canyon, px, py);
              const float depth = clamp01(dd / 16.0f);
              c = mul(c, 1.0f - 0.25f * depth);
              if (dd < 10.0f) {
                const float w = 1.0f - dd / 10.0f;
                c = mul(c, facing > 0 ? 1.0f - 0.55f * facing * w : 1.0f + 0.25f * (-facing) * w);
              }
              c = lerp(c, Rgb{64, 34, 40}, clamp01((dd - 6.0f) / 14.0f) * 0.8f);
              if (dd < 0.7f) c = lerp(c, Rgb{240, 170, 110}, 0.5f);
            }
          }
          break;
        }
        case kLTee:
          c = texel(tp.tee, px, py);
          c = mul(c, ((((int)floorf(wx / 2.0f)) + ((int)floorf(wy / 2.0f))) & 1) ? 1.06f : 0.97f);
          if (-d < 0.6f) c = mul(c, 0.88f);
          break;
        default: {                                                // green
          c = texel(tp.green, px, py);
          c = mul(c, ((((int)floorf(wx / 3.0f)) + ((int)floorf(wy / 3.0f))) & 1) ? 1.035f : 0.97f);
          const float u = (wx - def.greenX) / def.greenRx, v = (wy - def.greenY) / def.greenRy;
          c = mul(c, 1.0f + 0.09f * (-u * 0.6f + v * 0.8f) - 0.05f * clamp01(-d / gr));
          if (-d < 0.35f) c = mul(c, 0.92f);
          if (grid_ && d < 0) {
            const float fx = wx - 1.5f * floorf(wx / 1.5f), fy = wy - 1.5f * floorf(wy / 1.5f);
            if (fx < inv || fy < inv) c = lerp(c, Rgb{235, 255, 225}, 0.28f);
          }
          break;
        }
      }
      col = (li == top || L.cov >= 1) ? c : lerp(col, c, L.cov);
    }
    const int k = px >> 2;
    const float f = (px & 3) * 0.25f;
    col = mul(col, light[k] + (light[k + 1] - light[k]) * f);
    out[px] = pack(col);
  }
}

namespace {
struct Blitter {
  uint16_t* dst;
  int stride, w, h;
  void blend(int x, int y, const Rgb& c, float a) {
    if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
    uint16_t& d = dst[y * stride + x];
    d = pack(a >= 1 ? c : lerp(unpack(d), c, a));
  }
  void darken(int x, int y, float k) {
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    uint16_t& d = dst[y * stride + x];
    d = pack(mul(unpack(d), k));
  }
  void sprite(const golf_art::Sprite& sp, int x, int y, bool flip) {
    for (int j = 0; j < sp.h; ++j) {
      const int yy = y + j;
      if (yy < 0 || yy >= h) continue;
      for (int i = 0; i < sp.w; ++i) {
        const int si = j * sp.w + (flip ? sp.w - 1 - i : i);
        const uint8_t a = sp.a[si];
        if (!a) continue;
        const uint16_t p = sp.px[si];
        blend(x + i, yy, Rgb{(float)(((p >> 11) & 31) * 255 / 31), (float)(((p >> 5) & 63) * 255 / 63), (float)((p & 31) * 255 / 31)},
              a / 255.0f);
      }
    }
  }
  // soft shadow ellipse (multiply)
  void shadow(float cx, float cy, float rx, float ry, float strength) {
    const int x0 = (int)floorf(cx - rx - 1), x1 = (int)ceilf(cx + rx + 1);
    const int y0 = (int)floorf(cy - ry - 1), y1 = (int)ceilf(cy + ry + 1);
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
        const float d = sqrtf(dx * dx + dy * dy);
        const float cover = clamp01((1.0f - d) * (rx < ry ? rx : ry) * 0.6f + 0.3f);
        if (cover > 0) darken(x, y, 1.0f - strength * cover);
      }
  }
};
}  // namespace

void HoleRenderer::renderObjects() {
  const HoleMap& m = *map_;
  const float s = v_.scale;
  Blitter b = {dst_, stride_, v_.w, v_.h};
  // tree sizes and shadows
  const int nt = m.treeCount();
  uint8_t order[HoleMap::kMaxTrees];
  for (int i = 0; i < nt; ++i) order[i] = (uint8_t)i;
  for (int i = 1; i < nt; ++i) {                                  // far (top) first
    const uint8_t k = order[i];
    int j = i - 1;
    while (j >= 0 && m.tree(order[j]).y < m.tree(k).y) { order[j + 1] = order[j]; --j; }
    order[j + 1] = k;
  }
  for (int i = 0; i < nt; ++i) {
    const Tree& t = m.tree(i);
    const float bx = viewX(v_, t.x), by = viewY(v_, t.y);
    const float R = fminf(t.r * s * 1.25f, 16.0f);
    const float hpx = fminf(t.h * s * 1.25f, 40.0f);
    if (bx < -60 || bx > v_.w + 60 || by < -60 || by > v_.h + 60) continue;
    b.shadow(bx + 0.25f * hpx + 0.2f * R, by + 0.12f * hpx, R + 0.2f * hpx, 0.55f * R + 0.5f, 0.38f);
  }
  // out-of-bounds stakes where the course meets the outside
  if (m.theme() != Theme::Range) {
    for (int i = 0; i < m.shapeCount() && i < HoleMap::kMaxShapes; ++i) {
      const Shape& sh = m.shape(i);
      if (sh.surface != Surface::Rough && sh.surface != Surface::Waste) continue;
      const int samples = 120;
      for (int k = 0; k < samples; ++k) {
        float px, py, nx, ny;
        if (sh.kind == ShapeKind::Ellipse) {
          const float a = k * 6.2831853f / samples;
          const float c = cosf(sh.bx * 0.0174533f), sn = sinf(sh.bx * 0.0174533f);
          const float qx = cosf(a) * sh.ra, qy = sinf(a) * sh.rb;
          px = sh.ax + qx * c + qy * sn;
          py = sh.ay - qx * sn + qy * c;
          nx = (px - sh.ax); ny = (py - sh.ay);
        } else {
          const float dx = sh.bx - sh.ax, dy = sh.by - sh.ay, len = sqrtf(dx * dx + dy * dy) + 1e-3f;
          const float side = k < samples / 2 ? 1.0f : -1.0f;
          const float u = (k % (samples / 2)) / (float)(samples / 2 - 1);
          const float r = sh.ra + (sh.rb - sh.ra) * u;
          nx = -dy / len * side; ny = dx / len * side;
          px = sh.ax + dx * u + nx * r;
          py = sh.ay + dy * u + ny * r;
        }
        const float nl = sqrtf(nx * nx + ny * ny) + 1e-4f;
        nx /= nl; ny /= nl;
        // walk to the real (organic) edge
        const float d = m.edgeDistance(i, px, py);
        px -= nx * d; py -= ny * d;
        if (m.surfaceAt(px + nx * 2.0f, py + ny * 2.0f) != Surface::OutOfBounds) continue;
        if (m.surfaceAt(px - nx * 2.0f, py - ny * 2.0f) == Surface::OutOfBounds) continue;
        // one stake every ~10 yards: quantise along the boundary by position
        const int q = (int)floorf((px + py * 0.7f + 1000.0f) / 10.0f);
        const float fq = (px + py * 0.7f + 1000.0f) / 10.0f - q;
        if (fq > 0.3f) continue;
        const int sx = (int)lroundf(viewX(v_, px)), sy = (int)lroundf(viewY(v_, py));
        b.darken(sx + 1, sy, 0.6f);
        b.darken(sx + 1, sy + 1, 0.6f);
        for (int yy = sy - 3; yy <= sy; ++yy) b.blend(sx, yy, Rgb{250, 250, 250}, 1.0f);
      }
    }
  }
  // tee markers
  {
    const HoleDef& d = m.def();
    const ThemePaint& tp = kPaints[(int)m.theme()];
    for (int k = -1; k <= 1; k += 2) {
      const float cx = viewX(v_, d.teeX + k * 3.5f), cy = viewY(v_, d.teeY + 4.0f);
      const float r = fmaxf(1.2f, 0.7f * s);
      b.shadow(cx + r * 0.6f, cy + r * 0.4f, r, r * 0.8f, 0.4f);
      for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); ++y)
        for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); ++x) {
          const float dd = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy));
          const float cov = clamp01(r + 0.5f - dd);
          const float lit = clamp01(1.0f - ((x + 0.5f - cx) * -0.6f + (y + 0.5f - cy) * -0.8f) / (r + 0.01f) * 0.5f);
          b.blend(x, y, mul(Rgb{(float)tp.marker[0], (float)tp.marker[1], (float)tp.marker[2]}, 0.7f + 0.5f * (1 - lit)), cov);
        }
    }
  }
  // trees, far to near
  for (int oi = 0; oi < nt; ++oi) {
    const Tree& t = m.tree(order[oi]);
    const float bx = viewX(v_, t.x), by = viewY(v_, t.y);
    if (bx < -60 || bx > v_.w + 60 || by < -10 || by > v_.h + 80) continue;
    const float want = t.r * s * 1.25f;
    int best = 0;
    float bestErr = 1e9f;
    for (int k = 0; k < kTreeSizes; ++k) {
      const float err = fabsf(logf(kTreeSizePx[k] / want));
      if (err < bestErr) { bestErr = err; best = k; }
    }
    const golf_art::Sprite& sp = kTrees[(int)t.type * kTreeSizes + best];
    b.sprite(sp, (int)lroundf(bx) - sp.ax, (int)lroundf(by) - sp.ay, (t.variant & 1) != 0);
  }
}

void downsample(const uint16_t* src, int stride, int sw, int sh, uint16_t* dst, int dw, int dh) {
  for (int y = 0; y < dh; ++y) {
    const int sy0 = y * sh / dh, sy1 = ((y + 1) * sh / dh) > sy0 ? (y + 1) * sh / dh : sy0 + 1;
    for (int x = 0; x < dw; ++x) {
      const int sx0 = x * sw / dw, sx1 = ((x + 1) * sw / dw) > sx0 ? (x + 1) * sw / dw : sx0 + 1;
      uint32_t r = 0, g = 0, b = 0, n = 0;
      for (int yy = sy0; yy < sy1 && yy < sh; ++yy)
        for (int xx = sx0; xx < sx1 && xx < sw; ++xx) {
          const uint16_t sv = src[yy * stride + xx];
          const uint16_t v = (uint16_t)((sv >> 8) | (sv << 8));
          r += v >> 11; g += (v >> 5) & 63; b += v & 31; ++n;
        }
      if (!n) n = 1;
      const uint16_t v = (uint16_t)(((r / n) << 11) | ((g / n) << 5) | (b / n));
      dst[y * dw + x] = (uint16_t)((v >> 8) | (v << 8));
    }
  }
}

View makeOverview(const uint16_t* map, const View& mv, uint16_t* dst, int maxW, int h, int* outW) {
  const float k = (float)h / mv.h;                       // shrink factor
  int srcW = mv.w, srcX = 0;
  int w = (int)(mv.w * k + 0.5f);
  if (w > maxW) {
    w = maxW;
    srcW = (int)(maxW / k + 0.5f);
    if (srcW > mv.w) srcW = mv.w;
    srcX = (mv.w - srcW) / 2;
  }
  downsample(map + srcX, mv.w, srcW, mv.h, dst, w, h);
  View o;
  o.scale = mv.scale * k;
  o.x0 = mv.x0 + srcX / mv.scale;
  o.y0 = mv.y0;
  o.w = w;
  o.h = h;
  *outW = w;
  return o;
}

}  // namespace golf
}  // namespace ui
