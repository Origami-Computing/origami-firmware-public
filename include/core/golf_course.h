#pragma once
/**
 * Golf courses: the three nine-hole courses and the driving range as compact data, and the geometry queries the
 * game, the caddie and the renderer share (surface at a point, signed distances, trees).
 *
 * Hole frame: yards; +y from the tee toward the green (the top of the screen), +x to the right. Headings are
 * degrees clockwise from +y (0 = straight up the screen, 90 = right).
 * Pure C++ (no Arduino, no allocation): host-tested in test/test_golf.
 */
#include <stdint.h>

namespace core {
namespace golf {

enum class Surface : uint8_t { OutOfBounds, Rough, Fairway, Green, Tee, Bunker, Water, Waste, Canyon, Fringe };
const char* surfaceName(Surface s);
constexpr float kFringe = 2.5f;   // yards of fringe (collar) around every green
inline bool isPenalty(Surface s) { return s == Surface::Water || s == Surface::Canyon || s == Surface::OutOfBounds; }

enum class ShapeKind : uint8_t { Capsule, Ellipse };
// Capsule: segment A-B with radius ra at A and rb at B (tapered). Ellipse: centre A, radii ra (x) and rb (y),
// rotated clockwise by bx degrees. Later shapes lie on top of earlier ones; the corridor (the in-bounds area) is
// the union of every shape; outside it is out of bounds.
struct Shape {
  ShapeKind kind;
  Surface surface;
  int16_t ax, ay, bx, by;
  uint8_t ra, rb;
};

enum class TreeType : uint8_t { Oak, Pine, Birch, Gorse, Palm, Cactus, Rock, Count };
// `count` trees spread along A-B (jittered sideways by up to `jitter` yards); a single tree is count 1, A == B.
struct TreeLine {
  int16_t ax, ay, bx, by;
  uint8_t count;
  TreeType type;
  uint8_t jitter;
};

struct HoleDef {
  uint8_t par;
  int16_t teeX, teeY;
  int16_t pinX, pinY;
  int16_t greenX, greenY;       // green ellipse
  uint8_t greenRx, greenRy;
  int8_t greenRot;
  int8_t slopeX, slopeY;        // green slope: downhill acceleration, 1/100 yd/s^2
  uint8_t routeCount;           // turning points between tee and pin (doglegs); the yardage follows them
  int16_t route[3][2];
  const Shape* shapes;
  uint8_t shapeCount;
  const TreeLine* trees;
  uint8_t treeLineCount;
};

enum class Theme : uint8_t { Parkland, Links, Desert, Range };

struct CourseDef {
  const char* name;
  Theme theme;
  uint8_t windMinMph, windMaxMph;
  const HoleDef* holes;
  uint8_t holeCount;
};

constexpr int kCourseCount = 4;   // 0..2 are the nine-hole courses, 3 is the driving range
const CourseDef& course(int index);

struct Tree {
  float x, y, r, h;             // canopy radius and height (yards)
  TreeType type;
  uint8_t variant;              // 0..255, for art variety
};

// Signed distance from (x, y) to the shape's edge, yards (negative inside). Exact near the edge.
float shapeDistance(const Shape& s, float x, float y);
// Low-frequency value noise in [-1, 1] (deterministic); used to make edges organic, shared by physics and art.
float noise2(float x, float y, uint32_t seed);

// One hole, ready to play: generated trees and the queries.
class HoleMap {
 public:
  static constexpr int kMaxTrees = 96;
  static constexpr int kMaxShapes = 24;
  void load(const HoleDef& def, Theme theme, uint32_t seed);
  const HoleDef& def() const { return *def_; }
  Theme theme() const { return theme_; }

  Surface surfaceAt(float x, float y) const;
  // Distance inside the green (negative inside) including the organic edge.
  float greenDistance(float x, float y) const;
  bool onGreen(float x, float y) const { return greenDistance(x, y) <= 0; }

  int treeCount() const { return treeCount_; }
  const Tree& tree(int i) const { return trees_[i]; }

  float pinX() const { return def_->pinX; }
  float pinY() const { return def_->pinY; }
  float yards() const;                           // tee -> route -> pin
  float slopeX() const { return def_->slopeX / 100.0f; }
  float slopeY() const { return def_->slopeY / 100.0f; }
  // Map extents (yards) that hold everything worth drawing.
  float minX() const { return minX_; }
  float maxX() const { return maxX_; }
  float minY() const { return minY_; }
  float maxY() const { return maxY_; }
  // Route: tee, turning points, pin. point(i) for i in [0, routePoints()).
  int routePoints() const { return def_->routeCount + 2; }
  void routePoint(int i, float* x, float* y) const;

  // Per-shape distance with the organic edge applied (what surfaceAt uses; the renderer draws the same edges).
  float edgeDistance(int index, float x, float y) const;
  int shapeCount() const { return def_->shapeCount; }
  const Shape& shape(int i) const { return def_->shapes[i]; }
  // How far (yards) the organic edge can move a shape's border.
  static float edgeAmplitude(Surface s);
  // The tee box as a shape.
  Shape teeShape() const;

 private:
  float edge(const Shape& s, float c, float sn, float amp, uint32_t noiseSeed, float x, float y) const;
  const HoleDef* def_ = nullptr;
  Theme theme_ = Theme::Parkland;
  uint32_t seed_ = 0;
  float cos_[kMaxShapes + 1], sin_[kMaxShapes + 1];   // ellipse rotations (the last is the green)
  float amp_[kMaxShapes + 1];                         // organic edge amplitudes
  Tree trees_[kMaxTrees];
  int treeCount_ = 0;
  float minX_ = 0, maxX_ = 0, minY_ = 0, maxY_ = 0;
};

}  // namespace golf
}  // namespace core
