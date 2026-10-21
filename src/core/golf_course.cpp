// Golf courses and geometry: see include/core/golf_course.h.
#include "core/golf_course.h"

#include <math.h>

namespace core {
namespace golf {

namespace {
constexpr float kDeg = 3.14159265f / 180.0f;
}

const char* surfaceName(Surface s) {
  switch (s) {
    case Surface::OutOfBounds: return "Out of bounds";
    case Surface::Rough: return "Rough";
    case Surface::Fairway: return "Fairway";
    case Surface::Green: return "Green";
    case Surface::Tee: return "Tee";
    case Surface::Bunker: return "Bunker";
    case Surface::Water: return "Water";
    case Surface::Waste: return "Waste area";
    case Surface::Canyon: return "Canyon";
    case Surface::Fringe: return "Fringe";
  }
  return "?";
}

// ---------------- geometry ----------------

namespace {
float shapeDistanceCS(const Shape& s, float c, float sn, float x, float y) {
  if (s.kind == ShapeKind::Capsule) {
    const float dx = s.bx - s.ax, dy = s.by - s.ay;
    const float len2 = dx * dx + dy * dy;
    float t = len2 > 0 ? ((x - s.ax) * dx + (y - s.ay) * dy) / len2 : 0;
    t = t < 0 ? 0 : (t > 1 ? 1 : t);
    const float px = s.ax + dx * t - x, py = s.ay + dy * t - y;
    return sqrtf(px * px + py * py) - (s.ra + (s.rb - s.ra) * t);
  }
  // rotated ellipse (clockwise by bx degrees): implicit-function distance f / |grad f|
  const float ux = x - s.ax, uy = y - s.ay;
  const float qx = ux * c - uy * sn, qy = ux * sn + uy * c;
  const float irx = 1.0f / s.ra, iry = 1.0f / s.rb;
  const float k0 = sqrtf(qx * qx * irx * irx + qy * qy * iry * iry);
  if (k0 < 1e-4f) return -(s.ra < s.rb ? s.ra : s.rb);
  const float gx = qx * irx * irx, gy = qy * iry * iry;
  return k0 * (k0 - 1.0f) / sqrtf(gx * gx + gy * gy);
}
}  // namespace

float shapeDistance(const Shape& s, float x, float y) {
  return shapeDistanceCS(s, cosf(s.bx * kDeg), sinf(s.bx * kDeg), x, y);
}

namespace {
float hash01(int x, int y, uint32_t seed) {
  uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + seed * 2246822519u;
  h = (h ^ (h >> 13)) * 1274126177u;
  h ^= h >> 16;
  return (h & 0xFFFF) / 65535.0f;
}
uint32_t mix(uint32_t h) {
  h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
  return h;
}
}  // namespace

float noise2(float x, float y, uint32_t seed) {
  const float fx = floorf(x), fy = floorf(y);
  const int xi = (int)fx, yi = (int)fy;
  float tx = x - fx, ty = y - fy;
  tx = tx * tx * (3 - 2 * tx);
  ty = ty * ty * (3 - 2 * ty);
  const float a = hash01(xi, yi, seed), b = hash01(xi + 1, yi, seed);
  const float c = hash01(xi, yi + 1, seed), d = hash01(xi + 1, yi + 1, seed);
  const float v = (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
  return v * 2.0f - 1.0f;
}

float HoleMap::edgeAmplitude(Surface s) {
  switch (s) {
    case Surface::Rough: return 4.0f;
    case Surface::Fairway: return 1.3f;
    case Surface::Bunker: return 0.7f;
    case Surface::Water: return 1.6f;
    case Surface::Waste: return 2.5f;
    case Surface::Canyon: return 2.5f;
    case Surface::Green: return 0.3f;
    default: return 0.0f;
  }
}

namespace {
// Tree kinds: canopy radius, height (yards).
const float kTreeR[] = {6.0f, 4.5f, 4.0f, 3.0f, 4.2f, 1.8f, 3.0f};
const float kTreeH[] = {13.0f, 17.0f, 12.0f, 2.0f, 11.0f, 5.0f, 1.5f};

const Shape kTeeShapeProto = {ShapeKind::Capsule, Surface::Tee, 0, 0, 0, 0, 5, 5};
}  // namespace

float HoleMap::edge(const Shape& s, float c, float sn, float amp, uint32_t noiseSeed, float x, float y) const {
  const float d = shapeDistanceCS(s, c, sn, x, y);
  if (amp <= 0 || d > amp + 1.0f || d < -amp - 1.0f) return d;
  return d + amp * noise2(x * 0.11f, y * 0.11f, noiseSeed);
}

float HoleMap::edgeDistance(int i, float x, float y) const {
  return edge(def_->shapes[i], cos_[i], sin_[i], amp_[i], seed_ * 31u + (uint32_t)i * 7u, x, y);
}

float HoleMap::greenDistance(float x, float y) const {
  Shape g;
  g.kind = ShapeKind::Ellipse;
  g.surface = Surface::Green;
  g.ax = def_->greenX;
  g.ay = def_->greenY;
  g.bx = def_->greenRot;
  g.by = 0;
  g.ra = def_->greenRx;
  g.rb = def_->greenRy;
  return edge(g, cos_[kMaxShapes], sin_[kMaxShapes], amp_[kMaxShapes], seed_ * 31u + 99u * 7u, x, y);
}

Shape HoleMap::teeShape() const {
  Shape tee = kTeeShapeProto;
  tee.ax = tee.bx = def_->teeX;
  tee.ay = (int16_t)(def_->teeY - 5);
  tee.by = (int16_t)(def_->teeY + 5);
  return tee;
}

Surface HoleMap::surfaceAt(float x, float y) const {
  const float gd = greenDistance(x, y);
  if (gd <= 0) return Surface::Green;
  if (shapeDistance(teeShape(), x, y) <= 0) return Surface::Tee;
  // hazards lie on top of everything but the green and tee; then the fringe; then grass and waste (later on top)
  for (int i = def_->shapeCount - 1; i >= 0; --i) {
    const Surface s = def_->shapes[i].surface;
    if ((s == Surface::Bunker || s == Surface::Water || s == Surface::Canyon) && edgeDistance(i, x, y) <= 0) return s;
  }
  if (gd <= kFringe) return Surface::Fringe;
  // same order as the renderer: range greens, then fairways, then rough and waste (later on top)
  for (int pass = 0; pass < 3; ++pass)
    for (int i = def_->shapeCount - 1; i >= 0; --i) {
      const Surface s = def_->shapes[i].surface;
      if (s == Surface::Bunker || s == Surface::Water || s == Surface::Canyon) continue;
      const int rank = s == Surface::Green ? 0 : (s == Surface::Fairway ? 1 : 2);
      if (rank == pass && edgeDistance(i, x, y) <= 0) return s;
    }
  return Surface::OutOfBounds;
}

void HoleMap::routePoint(int i, float* x, float* y) const {
  if (i <= 0) { *x = def_->teeX; *y = def_->teeY; return; }
  if (i > def_->routeCount) { *x = def_->pinX; *y = def_->pinY; return; }
  *x = def_->route[i - 1][0];
  *y = def_->route[i - 1][1];
}

float HoleMap::yards() const {
  float total = 0, px, py;
  routePoint(0, &px, &py);
  for (int i = 1; i < routePoints(); ++i) {
    float x, y;
    routePoint(i, &x, &y);
    total += sqrtf((x - px) * (x - px) + (y - py) * (y - py));
    px = x;
    py = y;
  }
  return total;
}

void HoleMap::load(const HoleDef& def, Theme theme, uint32_t seed) {
  def_ = &def;
  theme_ = theme;
  seed_ = seed;
  for (int i = 0; i < def.shapeCount && i < kMaxShapes; ++i) {
    cos_[i] = cosf(def.shapes[i].bx * kDeg);
    sin_[i] = sinf(def.shapes[i].bx * kDeg);
    amp_[i] = edgeAmplitude(def.shapes[i].surface);
  }
  amp_[kMaxShapes] = edgeAmplitude(Surface::Green);
  cos_[kMaxShapes] = cosf(def.greenRot * kDeg);
  sin_[kMaxShapes] = sinf(def.greenRot * kDeg);
  treeCount_ = 0;
  minX_ = maxX_ = def.teeX;
  minY_ = maxY_ = def.teeY;
  // trees: spread along each line, jittered; never on a fairway, green, tee, bunker or hazard
  for (int l = 0; l < def.treeLineCount; ++l) {
    const TreeLine& t = def.trees[l];
    const float dx = t.bx - t.ax, dy = t.by - t.ay;
    const float len = sqrtf(dx * dx + dy * dy);
    const float nx = len > 0 ? -dy / len : 1, ny = len > 0 ? dx / len : 0;
    for (int i = 0; i < t.count && treeCount_ < kMaxTrees; ++i) {
      const uint32_t h = mix(seed * 7919u + (uint32_t)l * 104729u + (uint32_t)i * 2654435761u);
      const float u = t.count > 1 ? (float)i / (t.count - 1) : 0.0f;
      const float along = t.count > 1 ? ((h & 0xFF) / 255.0f - 0.5f) * 0.6f / (t.count - 1) : 0.0f;
      const float side = (((h >> 8) & 0xFF) / 255.0f - 0.5f) * 2.0f * t.jitter;
      const float uu = u + along < 0 ? 0 : (u + along > 1 ? 1 : u + along);
      const float x = t.ax + dx * uu + nx * side, y = t.ay + dy * uu + ny * side;
      const Surface s = surfaceAt(x, y);
      if (s != Surface::Rough && s != Surface::Waste && s != Surface::OutOfBounds) continue;
      if (greenDistance(x, y) < kTreeR[(int)t.type] + 2.0f) continue;
      Tree& tr = trees_[treeCount_++];
      const float size = 0.82f + (((h >> 16) & 0xFF) / 255.0f) * 0.36f;
      tr.x = x;
      tr.y = y;
      tr.r = kTreeR[(int)t.type] * size;
      tr.h = kTreeH[(int)t.type] * size;
      tr.type = t.type;
      tr.variant = (uint8_t)(h >> 24);
    }
  }
  // The area worth showing: the route and the green, with room either side (hazards beyond are scenery).
  for (int i = 0; i < routePoints(); ++i) {
    float x, y;
    routePoint(i, &x, &y);
    if (x < minX_) minX_ = x;
    if (x > maxX_) maxX_ = x;
    if (y < minY_) minY_ = y;
    if (y > maxY_) maxY_ = y;
  }
  minX_ = fminf(minX_, def.greenX - def.greenRx) - 45;
  maxX_ = fmaxf(maxX_, def.greenX + def.greenRx) + 45;
  minY_ -= 22;
  maxY_ = fmaxf(maxY_, def.greenY + def.greenRy) + 28;
}

// ---------------- the courses ----------------
// Helpers: a tapered capsule A-B (radius ra -> rb), an ellipse (centre, radii, clockwise rotation).
#define CAP(S, ax, ay, bx, by, ra, rb) {ShapeKind::Capsule, Surface::S, ax, ay, bx, by, ra, rb}
#define ELL(S, cx, cy, rx, ry, rot) {ShapeKind::Ellipse, Surface::S, cx, cy, rot, 0, rx, ry}
#define TREES(ax, ay, bx, by, n, T, j) {ax, ay, bx, by, n, TreeType::T, j}
#define N(a) (uint8_t)(sizeof(a) / sizeof(a[0]))
#define R0 0, {{0, 0}, {0, 0}, {0, 0}}

// ======== 1. Willow Park: parkland, tree-lined, bunkers, a creek and ponds ========
namespace park {
const Shape s1[] = {
    CAP(Rough, 0, -20, 4, 395, 45, 45),
    CAP(Fairway, 0, 40, 2, 345, 18, 16),
    ELL(Bunker, 25, 235, 9, 6, 20),
    ELL(Bunker, -17, 358, 6, 10, 15),
    ELL(Bunker, 22, 376, 6, 9, -10),
};
const TreeLine t1[] = {
    TREES(-50, 10, -46, 390, 13, Oak, 7), TREES(-68, 30, -66, 380, 8, Pine, 6),
    TREES(52, 10, 50, 390, 13, Oak, 7), TREES(70, 30, 70, 380, 8, Birch, 6),
    TREES(-30, 405, 40, 408, 6, Pine, 4),
};
const Shape s2[] = {
    CAP(Rough, 10, -20, 0, 250, 50, 50),
    CAP(Rough, 0, 250, -72, 408, 50, 45),
    CAP(Fairway, 8, 40, 0, 248, 17, 19),
    CAP(Fairway, 0, 248, -60, 372, 19, 15),
    CAP(Water, -125, 332, 45, 316, 4, 4),
    ELL(Bunker, -50, 408, 6, 8, 20),
    ELL(Bunker, -93, 388, 8, 5, -30),
    ELL(Bunker, 28, 262, 7, 11, 10),
};
const TreeLine t2[] = {
    TREES(-44, 190, -40, 285, 6, Oak, 5), TREES(-40, 20, -46, 170, 6, Birch, 5),
    TREES(48, 60, 42, 390, 12, Pine, 6), TREES(-105, 425, -35, 440, 6, Oak, 5),
    TREES(-110, 290, -115, 380, 4, Oak, 6),
};
const Shape s3[] = {
    CAP(Rough, 0, -20, -4, 192, 55, 50),
    ELL(Water, -2, 108, 42, 27, 8),
    CAP(Fairway, -5, 143, -5, 147, 12, 12),
    ELL(Bunker, 13, 180, 8, 5, -20),
    ELL(Bunker, -24, 160, 5, 8, 10),
};
const TreeLine t3[] = {
    TREES(-60, 0, -58, 190, 7, Oak, 6), TREES(58, 0, 54, 190, 7, Birch, 6), TREES(-40, 205, 32, 208, 5, Pine, 4),
};
const Shape s4[] = {
    CAP(Rough, 0, -20, 25, 250, 55, 55),
    CAP(Rough, 25, 250, -15, 400, 55, 55),
    CAP(Rough, -15, 400, 5, 540, 55, 45),
    CAP(Fairway, 0, 40, 25, 250, 16, 19),
    CAP(Fairway, 25, 250, -15, 400, 18, 15),
    CAP(Fairway, -15, 400, 3, 496, 15, 13),
    ELL(Water, 58, 360, 18, 34, -10),
    ELL(Bunker, -3, 232, 8, 6, 10),
    ELL(Bunker, 14, 410, 6, 9, 0),
    ELL(Bunker, -11, 522, 5, 9, 10),
    ELL(Bunker, 21, 508, 6, 8, -15),
};
const TreeLine t4[] = {
    TREES(-42, 20, -50, 330, 11, Oak, 6), TREES(-60, 340, -45, 520, 6, Pine, 6),
    TREES(58, 30, 64, 250, 7, Pine, 6), TREES(92, 300, 45, 530, 7, Oak, 6),
    TREES(-30, 555, 40, 552, 5, Birch, 4),
};
const Shape s5[] = {
    CAP(Rough, -5, -20, 8, 372, 48, 48),
    CAP(Fairway, -4, 40, 6, 318, 17, 15),
    ELL(Bunker, -19, 225, 7, 10, 5),
    ELL(Bunker, 23, 240, 7, 12, -5),
    ELL(Bunker, 7, 327, 10, 4, 0),
    ELL(Bunker, 25, 350, 5, 8, 10),
};
const TreeLine t5[] = {
    TREES(-45, 10, -40, 365, 12, Pine, 5), TREES(50, 10, 45, 365, 12, Oak, 5), TREES(-20, 380, 40, 382, 4, Oak, 3),
};
const Shape s6[] = {
    CAP(Rough, 0, -20, 10, 225, 45, 45),
    CAP(Fairway, 5, 150, 9, 176, 12, 13),
    ELL(Bunker, -8, 182, 6, 9, 15),
    ELL(Bunker, 29, 196, 6, 10, -10),
    ELL(Bunker, 8, 215, 10, 4, 0),
};
const TreeLine t6[] = {
    TREES(-36, 20, -30, 215, 8, Oak, 5), TREES(50, 20, 54, 215, 7, Birch, 5),
};
const Shape s7[] = {
    CAP(Rough, -20, -20, -10, 240, 52, 52),
    CAP(Rough, -10, 240, 82, 402, 50, 48),
    CAP(Fairway, -19, 40, -10, 238, 17, 20),
    CAP(Fairway, -10, 238, 68, 364, 18, 14),
    ELL(Water, 42, 196, 25, 32, 10),
    ELL(Bunker, -38, 262, 8, 10, 20),
    ELL(Bunker, 62, 394, 6, 8, 30),
    ELL(Bunker, 101, 371, 5, 8, 20),
};
const TreeLine t7[] = {
    TREES(-58, 20, -55, 300, 10, Oak, 5), TREES(-45, 315, 40, 432, 7, Pine, 5),
    TREES(82, 40, 98, 160, 6, Birch, 5), TREES(124, 320, 128, 412, 4, Oak, 5),
};
const Shape s8[] = {
    CAP(Rough, -10, -20, -10, 572, 55, 55),
    CAP(Fairway, 0, 40, -5, 285, 18, 18),
    CAP(Fairway, -5, 332, -8, 470, 17, 15),
    CAP(Water, -70, 292, 60, 318, 4, 4),
    CAP(Water, -70, 494, 55, 486, 4, 4),
    ELL(Bunker, 20, 420, 8, 6, -10),
    ELL(Bunker, -30, 546, 6, 9, 10),
    ELL(Bunker, 11, 530, 6, 8, -10),
};
const TreeLine t8[] = {
    TREES(-52, 30, -56, 560, 17, Oak, 5), TREES(40, 30, 36, 560, 16, Pine, 4),
};
const Shape s9[] = {
    CAP(Rough, 15, -20, 10, 262, 52, 52),
    CAP(Rough, 10, 262, -52, 442, 58, 55),
    CAP(Fairway, 14, 40, 10, 258, 17, 19),
    CAP(Fairway, 10, 258, -42, 400, 18, 14),
    ELL(Water, -100, 388, 20, 30, 10),
    ELL(Bunker, 36, 250, 8, 10, 0),
    ELL(Bunker, -27, 433, 6, 8, -20),
    ELL(Bunker, -67, 404, 6, 5, 30),
};
const TreeLine t9[] = {
    TREES(55, 20, 60, 420, 12, Oak, 5), TREES(-30, 30, -36, 200, 7, Pine, 5), TREES(-95, 455, -10, 458, 6, Oak, 4),
};
const HoleDef holes[] = {
    {4, 0, 0, 4, 368, 4, 370, 14, 17, 0, -8, -5, R0, s1, N(s1), t1, N(t1)},
    {4, 10, 0, -72, 396, -72, 398, 15, 13, -30, 6, -4, 1, {{0, 240}, {0, 0}, {0, 0}}, s2, N(s2), t2, N(t2)},
    {3, 0, 0, -6, 163, -5, 165, 16, 13, 10, -5, -6, R0, s3, N(s3), t3, N(t3)},
    {5, 0, 0, 5, 516, 5, 518, 13, 16, 10, 4, -7, 2, {{25, 250}, {-15, 400}, {0, 0}}, s4, N(s4), t4, N(t4)},
    {4, -5, 0, 9, 346, 8, 345, 12, 15, -10, -6, -3, R0, s5, N(s5), t5, N(t5)},
    {3, 0, 0, 10, 193, 10, 195, 14, 15, 0, 0, -8, R0, s6, N(s6), t6, N(t6)},
    {4, -20, 0, 82, 381, 82, 383, 15, 13, 30, -5, -5, 1, {{-10, 230}, {0, 0}, {0, 0}}, s7, N(s7), t7, N(t7)},
    {5, 0, 0, -10, 538, -10, 540, 14, 16, 0, 7, -6, R0, s8, N(s8), t8, N(t8)},
    {4, 15, 0, -50, 421, -50, 423, 16, 14, -20, -4, -6, 1, {{10, 250}, {0, 0}, {0, 0}}, s9, N(s9), t9, N(t9)},
};
}  // namespace park

// ======== 2. Gull Point Links: seaside, fescue, pot bunkers, dunes, gorse, wind ========
namespace links {
const Shape s1[] = {
    CAP(Rough, 15, -20, 10, 405, 70, 70),
    ELL(Waste, 88, 200, 20, 60, 5),
    CAP(Fairway, 18, 40, 10, 352, 22, 22),
    CAP(Waste, -52, -40, -52, 460, 12, 12),
    CAP(Water, -150, -40, -150, 460, 95, 95),
    ELL(Bunker, 0, 230, 4, 4, 0),
    ELL(Bunker, 31, 260, 4, 5, 0),
    ELL(Bunker, -3, 300, 5, 4, 0),
    ELL(Bunker, -6, 368, 5, 5, 0),
    ELL(Bunker, 26, 386, 4, 4, 0),
};
const TreeLine t1[] = {TREES(62, 40, 72, 380, 10, Gorse, 6), TREES(50, 395, 80, 420, 4, Gorse, 4)};
const Shape s2[] = {
    CAP(Rough, -30, -20, -20, 270, 65, 65),
    CAP(Rough, -20, 270, 60, 515, 65, 60),
    ELL(Waste, 20, 170, 24, 48, 10),
    ELL(Waste, -78, 300, 18, 60, 0),
    CAP(Fairway, -29, 40, -20, 268, 20, 20),
    CAP(Fairway, -20, 268, 55, 472, 18, 15),
    ELL(Bunker, -12, 238, 4, 4, 0),
    ELL(Bunker, -42, 282, 5, 4, 0),
    ELL(Bunker, 18, 382, 4, 5, 0),
    ELL(Bunker, 40, 424, 4, 4, 0),
    ELL(Bunker, 49, 474, 4, 4, 0),
    ELL(Bunker, 79, 488, 4, 5, 0),
};
const TreeLine t2[] = {
    TREES(14, 220, 32, 330, 5, Gorse, 5), TREES(-80, 20, -75, 240, 7, Gorse, 6), TREES(95, 330, 110, 480, 5, Gorse, 6),
};
const Shape s3[] = {
    CAP(Rough, 0, -20, -5, 200, 55, 55),
    CAP(Water, 132, -40, 92, 230, 80, 80),
    ELL(Bunker, -25, 178, 5, 5, 0),
    ELL(Bunker, -19, 159, 4, 4, 0),
};
const TreeLine t3[] = {TREES(-52, 20, -46, 190, 6, Gorse, 5)};
const Shape s4[] = {
    CAP(Rough, 0, -20, 5, 446, 70, 70),
    ELL(Waste, -72, 150, 18, 40, 0),
    CAP(Fairway, 0, 40, 4, 384, 22, 22),
    CAP(Water, -75, 392, 80, 396, 3, 3),
    ELL(Bunker, -15, 250, 4, 4, 0),
    ELL(Bunker, 19, 266, 4, 5, 0),
    ELL(Bunker, -5, 302, 4, 4, 0),
    ELL(Bunker, 23, 428, 4, 4, 0),
};
const TreeLine t4[] = {TREES(-56, 30, -60, 420, 9, Gorse, 5), TREES(60, 30, 66, 420, 9, Gorse, 5)};
const Shape s5[] = {
    CAP(Rough, -35, -20, 30, 382, 75, 70),
    CAP(Fairway, -38, 40, 25, 332, 20, 20),
    CAP(Waste, 70, -20, 70, 420, 10, 10),
    CAP(Water, 162, -20, 162, 420, 95, 95),
    ELL(Bunker, 0, 214, 4, 4, 0),
    ELL(Bunker, -11, 251, 5, 4, 0),
    ELL(Bunker, 41, 341, 4, 4, 0),
    ELL(Bunker, 11, 369, 4, 4, 0),
};
const TreeLine t5[] = {TREES(-82, 40, -44, 360, 9, Gorse, 6)};
const Shape s6[] = {
    CAP(Rough, 0, -20, 2, 172, 55, 55),
    ELL(Waste, 0, 72, 34, 38, 0),
    ELL(Bunker, -13, 142, 4, 6, 0),
    ELL(Bunker, 15, 147, 4, 6, 0),
    ELL(Bunker, 0, 130, 6, 3, 0),
    ELL(Bunker, 3, 159, 5, 3, 0),
};
const TreeLine t6[] = {TREES(-46, 0, -40, 170, 5, Gorse, 5), TREES(46, 0, 40, 170, 5, Gorse, 5)};
const Shape s7[] = {
    CAP(Rough, 40, -20, 20, 290, 70, 70),
    CAP(Rough, 20, 290, -20, 552, 70, 70),
    CAP(Fairway, 38, 40, 20, 288, 22, 22),
    CAP(Fairway, 20, 288, -15, 502, 20, 16),
    CAP(Waste, -60, -40, -46, 560, 11, 11),
    CAP(Water, -170, -40, -150, 560, 100, 100),
    ELL(Bunker, 51, 250, 4, 5, 0),
    ELL(Bunker, 6, 265, 4, 4, 0),
    ELL(Bunker, 30, 330, 5, 4, 0),
    ELL(Bunker, -4, 400, 4, 4, 0),
    ELL(Bunker, 12, 430, 4, 4, 0),
    ELL(Bunker, -37, 520, 4, 5, 0),
    ELL(Bunker, -5, 542, 4, 4, 0),
};
const TreeLine t7[] = {TREES(92, 40, 62, 520, 12, Gorse, 6)};
const Shape s8[] = {
    CAP(Rough, 20, -20, 30, 240, 60, 60),
    CAP(Rough, 30, 240, -40, 416, 62, 60),
    ELL(Waste, -32, 196, 28, 52, 0),
    CAP(Fairway, 20, 40, 30, 238, 20, 20),
    CAP(Fairway, 30, 238, -35, 372, 18, 16),
    ELL(Bunker, 51, 246, 5, 5, 0),
    ELL(Bunker, -10, 330, 4, 4, 0),
    ELL(Bunker, -59, 396, 4, 5, 0),
    ELL(Bunker, -24, 402, 4, 4, 0),
};
const TreeLine t8[] = {TREES(72, 40, 76, 300, 8, Gorse, 5), TREES(-92, 300, -82, 420, 4, Gorse, 5)};
const Shape s9[] = {
    CAP(Rough, 0, -20, 0, 446, 70, 70),
    CAP(Fairway, 0, 40, 0, 388, 24, 24),
    CAP(Water, -82, 300, 72, 275, 3, 3),
    CAP(Waste, -200, 446, 200, 446, 7, 7),
    CAP(Water, -200, 472, 200, 472, 30, 30),
    ELL(Bunker, -21, 240, 4, 5, 0),
    ELL(Bunker, 26, 256, 4, 4, 0),
    ELL(Bunker, -13, 395, 4, 4, 0),
    ELL(Bunker, 16, 400, 4, 4, 0),
};
const TreeLine t9[] = {TREES(-62, 40, -66, 380, 7, Gorse, 5), TREES(66, 40, 62, 380, 7, Gorse, 5)};
const HoleDef holes[] = {
    {4, 20, 0, 10, 378, 10, 380, 18, 15, 0, -8, -4, R0, s1, N(s1), t1, N(t1)},
    {5, -30, 0, 60, 490, 60, 492, 16, 14, 20, 5, -7, 1, {{-20, 260}, {0, 0}, {0, 0}}, s2, N(s2), t2, N(t2)},
    {3, 0, 0, -5, 173, -5, 175, 14, 16, -10, 5, -3, R0, s3, N(s3), t3, N(t3)},
    {4, 0, 0, 5, 418, 5, 420, 16, 14, 0, -6, -6, R0, s4, N(s4), t4, N(t4)},
    {4, -40, 0, 30, 355, 30, 357, 15, 15, 0, 6, -4, R0, s5, N(s5), t5, N(t5)},
    {3, 0, 0, 2, 144, 2, 145, 9, 11, 0, 0, -5, R0, s6, N(s6), t6, N(t6)},
    {5, 40, 0, -20, 525, -20, 528, 16, 15, -15, -5, -6, 1, {{20, 280}, {0, 0}, {0, 0}}, s7, N(s7), t7, N(t7)},
    {4, 20, 0, -40, 390, -40, 393, 15, 14, -25, 4, -6, 1, {{30, 230}, {0, 0}, {0, 0}}, s8, N(s8), t8, N(t8)},
    {4, 0, 0, 0, 412, 0, 415, 18, 16, 0, 0, -7, R0, s9, N(s9), t9, N(t9)},
};
}  // namespace links

// ======== 3. Red Mesa Canyon: desert target golf, island fairways, canyon carries ========
namespace canyon {
const Shape s1[] = {
    CAP(Waste, 0, -20, 10, 386, 60, 60),
    CAP(Rough, 2, 140, 9, 346, 26, 22),
    CAP(Fairway, 2, 150, 9, 336, 20, 16),
    CAP(Canyon, -130, 90, 130, 80, 17, 17),
    ELL(Bunker, -9, 350, 5, 7, 10),
    ELL(Bunker, 29, 365, 5, 7, -10),
};
const TreeLine t1[] = {
    TREES(-45, 120, -40, 380, 8, Cactus, 6), TREES(48, 120, 50, 380, 8, Cactus, 6), TREES(-66, 190, -62, 310, 3, Rock, 6),
    TREES(-22, -12, 22, -12, 2, Palm, 0),
};
const Shape s2[] = {
    CAP(Waste, 0, -20, -6, 196, 55, 55),
    CAP(Canyon, -140, 96, 140, 104, 44, 44),
    ELL(Bunker, 10, 185, 6, 4, 0),
    ELL(Bunker, -29, 168, 5, 7, 0),
};
const TreeLine t2[] = {
    TREES(-55, 10, -50, 195, 5, Cactus, 5), TREES(50, 10, 55, 195, 5, Cactus, 5), TREES(-50, 165, 36, 165, 3, Rock, 4),
};
const Shape s3[] = {
    CAP(Waste, 0, -20, -20, 270, 60, 60),
    CAP(Waste, -20, 270, 20, 546, 60, 60),
    CAP(Rough, 0, 140, -18, 282, 26, 26),
    CAP(Rough, -15, 344, 12, 482, 24, 24),
    CAP(Fairway, 0, 150, -18, 272, 20, 20),
    CAP(Fairway, -15, 352, 12, 470, 18, 18),
    CAP(Canyon, -130, 314, 130, 324, 15, 15),
    ELL(Bunker, -41, 250, 6, 9, 0),
    ELL(Bunker, 16, 471, 7, 5, 0),
    ELL(Bunker, 4, 531, 5, 7, 0),
    ELL(Bunker, 39, 515, 5, 6, 0),
};
const TreeLine t3[] = {
    TREES(-58, 60, -65, 290, 7, Cactus, 6), TREES(45, 60, 40, 290, 7, Cactus, 6), TREES(60, 200, 70, 420, 4, Rock, 6),
    TREES(-60, 360, -50, 520, 5, Cactus, 6), TREES(40, 548, -5, 553, 3, Palm, 2),
};
const Shape s4[] = {
    CAP(Waste, -40, -20, -30, 240, 55, 55),
    CAP(Waste, -30, 240, 62, 396, 55, 55),
    CAP(Rough, -39, 36, -30, 240, 26, 25),
    CAP(Rough, -30, 240, 50, 360, 24, 22),
    CAP(Fairway, -39, 40, -30, 236, 20, 20),
    CAP(Fairway, -30, 236, 50, 356, 18, 16),
    ELL(Canyon, 42, 186, 34, 44, 20),
    ELL(Bunker, -62, 255, 7, 9, 0),
    ELL(Bunker, 45, 386, 5, 5, 0),
    ELL(Bunker, 81, 360, 5, 7, 0),
};
const TreeLine t4[] = {
    TREES(-86, 20, -80, 280, 7, Cactus, 6), TREES(-40, 300, 20, 400, 4, Cactus, 6), TREES(95, 300, 100, 400, 3, Rock, 5),
};
const Shape s5[] = {
    CAP(Waste, 0, -20, -15, 366, 55, 55),
    CAP(Rough, 0, 36, -5, 286, 24, 24),
    CAP(Fairway, 0, 40, -5, 282, 18, 18),
    ELL(Water, -32, 305, 28, 17, 0),
    ELL(Bunker, 9, 340, 5, 8, 0),
};
const TreeLine t5[] = {
    TREES(-68, 285, -48, 330, 4, Palm, 3), TREES(-45, 30, -50, 260, 6, Cactus, 5), TREES(45, 30, 40, 330, 7, Cactus, 5),
};
const Shape s6[] = {
    CAP(Waste, 0, -20, 15, 232, 60, 60),
    CAP(Rough, 12, 160, 14, 186, 18, 18),
    CAP(Fairway, 12, 165, 14, 183, 14, 14),
    CAP(Canyon, -95, 50, -40, 240, 24, 24),
    CAP(Canyon, -60, 248, 90, 252, 12, 12),
    ELL(Bunker, 37, 200, 5, 8, 0),
};
const TreeLine t6[] = {TREES(40, 10, 55, 190, 6, Cactus, 6), TREES(-20, 10, -10, 140, 4, Cactus, 5)};
const Shape s7[] = {
    CAP(Waste, 0, -20, -10, 436, 55, 55),
    CAP(Rough, 0, 112, 0, 268, 26, 26),
    CAP(Rough, -5, 300, -9, 392, 23, 23),
    CAP(Fairway, 0, 120, 0, 262, 20, 20),
    CAP(Fairway, -5, 305, -9, 386, 17, 17),
    CAP(Canyon, -130, 55, 130, 58, 12, 12),
    CAP(Canyon, -130, 286, 130, 282, 12, 12),
    ELL(Bunker, -29, 405, 5, 7, 0),
    ELL(Bunker, 9, 416, 5, 6, 0),
};
const TreeLine t7[] = {
    TREES(-45, 90, -45, 260, 5, Cactus, 6), TREES(45, 90, 45, 260, 5, Cactus, 6), TREES(-50, 310, -40, 420, 4, Cactus, 5),
    TREES(40, 310, 45, 420, 3, Rock, 5),
};
const Shape s8[] = {
    CAP(Waste, 20, -20, 0, 282, 60, 60),
    CAP(Waste, 0, 282, -40, 572, 60, 60),
    CAP(Rough, 19, 112, 0, 280, 28, 26),
    CAP(Rough, -5, 344, -35, 486, 24, 24),
    CAP(Fairway, 19, 120, 0, 275, 22, 20),
    CAP(Fairway, -5, 350, -35, 480, 18, 18),
    CAP(Canyon, -130, 330, 120, 292, 16, 16),
    ELL(Bunker, 31, 262, 6, 8, 0),
    ELL(Bunker, -16, 481, 6, 5, 0),
    ELL(Bunker, -58, 540, 5, 7, 0),
    ELL(Bunker, -22, 556, 5, 5, 0),
};
const TreeLine t8[] = {
    TREES(-40, 40, -50, 270, 6, Cactus, 6), TREES(60, 40, 50, 260, 6, Cactus, 6), TREES(30, 360, 20, 500, 4, Rock, 6),
    TREES(-75, 380, -80, 520, 4, Cactus, 5), TREES(-70, 560, -10, 572, 3, Palm, 3),
};
const Shape s9[] = {
    CAP(Waste, -10, -20, -10, 250, 60, 60),
    CAP(Waste, -10, 250, 40, 446, 60, 60),
    CAP(Rough, -10, 36, -10, 245, 26, 26),
    CAP(Rough, -10, 245, 30, 396, 23, 22),
    CAP(Fairway, -10, 40, -10, 245, 20, 20),
    CAP(Fairway, -10, 245, 30, 392, 17, 16),
    CAP(Canyon, 112, 150, 94, 480, 45, 45),
    CAP(Canyon, -40, 472, 110, 472, 25, 25),
    ELL(Bunker, 18, 424, 5, 7, 0),
    ELL(Bunker, -36, 262, 7, 9, 0),
};
const TreeLine t9[] = {
    TREES(-60, 30, -62, 300, 7, Cactus, 6), TREES(30, 40, 35, 200, 4, Cactus, 6), TREES(-45, 330, -20, 430, 3, Palm, 4),
    TREES(-70, 330, -60, 420, 3, Rock, 6),
};
const HoleDef holes[] = {
    {4, 0, 0, 10, 357, 10, 360, 15, 14, 0, -5, -5, R0, s1, N(s1), t1, N(t1)},
    {3, 0, 0, -8, 168, -8, 170, 16, 14, 0, 5, -6, R0, s2, N(s2), t2, N(t2)},
    {5, 0, 0, 20, 520, 20, 523, 14, 15, 10, -6, -4, 1, {{-20, 260}, {0, 0}, {0, 0}}, s3, N(s3), t3, N(t3)},
    {4, -40, 0, 60, 370, 62, 373, 14, 14, 0, -5, -5, 1, {{-30, 230}, {0, 0}, {0, 0}}, s4, N(s4), t4, N(t4)},
    {4, 0, 0, -15, 338, -15, 340, 14, 13, 0, 4, -6, R0, s5, N(s5), t5, N(t5)},
    {3, 0, 0, 15, 203, 15, 205, 16, 16, 0, -3, -8, R0, s6, N(s6), t6, N(t6)},
    {4, 0, 0, -10, 408, -10, 410, 15, 14, 0, 5, -5, R0, s7, N(s7), t7, N(t7)},
    {5, 20, 0, -40, 540, -40, 543, 15, 16, -10, -4, -6, 1, {{0, 270}, {0, 0}, {0, 0}}, s8, N(s8), t8, N(t8)},
    {4, -10, 0, 40, 415, 40, 418, 15, 15, 0, 6, -5, 1, {{-10, 240}, {0, 0}, {0, 0}}, s9, N(s9), t9, N(t9)},
};
}  // namespace canyon

// ======== Driving range: target greens at 50..250 yards ========
namespace range {
const Shape s1[] = {
    CAP(Rough, 0, -20, 0, 300, 110, 110),
    CAP(Fairway, 0, 20, 0, 285, 60, 60),
    ELL(Green, -15, 50, 8, 8, 0),
    ELL(Green, 15, 100, 10, 10, 0),
    ELL(Green, -20, 150, 12, 12, 0),
    ELL(Green, 20, 200, 14, 14, 0),
};
const TreeLine t1[] = {TREES(-100, 0, -100, 300, 11, Oak, 8), TREES(100, 0, 100, 300, 11, Pine, 8),
                       TREES(-80, 330, 80, 330, 8, Oak, 6)};
const HoleDef holes[] = {
    {4, 0, 0, 0, 250, 0, 250, 15, 15, 0, 0, 0, R0, s1, N(s1), t1, N(t1)},
};
}  // namespace range

namespace {
const CourseDef kCourses[kCourseCount] = {
    {"Willow Park", Theme::Parkland, 0, 8, park::holes, N(park::holes)},
    {"Gull Point Links", Theme::Links, 6, 20, links::holes, N(links::holes)},
    {"Red Mesa Canyon", Theme::Desert, 2, 14, canyon::holes, N(canyon::holes)},
    {"Driving Range", Theme::Range, 0, 0, range::holes, N(range::holes)},
};
}  // namespace

const CourseDef& course(int i) { return kCourses[i < 0 ? 0 : (i >= kCourseCount ? kCourseCount - 1 : i)]; }

}  // namespace golf
}  // namespace core
