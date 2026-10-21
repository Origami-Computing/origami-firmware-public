#pragma once
/**
 * Golf hole renderer: draws a hole top-down from the course data into an RGB565 buffer (byte-swapped, canvas
 * order), textured (assets/golf_art.h detail textures, per-course colours), anti-aliased, shaded (mowing stripes,
 * bunker lips, water depth, canyon walls, green domes), with trees and their shadows, out-of-bounds stakes and
 * tee markers. Incremental (a band of rows per call) so the UI task never blocks for long.
 * Pure C++: the host preview (test/test_golf/render_host.cpp) renders the same pixels as the device.
 */
#include <stdint.h>

#include "core/golf_course.h"

namespace ui {
namespace golf {
using namespace core::golf;

// World <-> pixel: x = x0 + px / scale, y = y0 - py / scale (the top of the buffer is the far end of the hole).
struct View {
  float x0, y0, scale;
  int w, h;
};

inline float viewX(const View& v, float wx) { return (wx - v.x0) * v.scale; }
inline float viewY(const View& v, float wy) { return (v.y0 - wy) * v.scale; }

// The whole hole in a buffer `w` wide, at most `maxH` tall and `maxScale` px per yard.
View holeView(const HoleMap& map, int w, int maxH, float maxScale);
// The green (and a little around it) in a w x h view.
View greenView(const HoleMap& map, int w, int h);

class HoleRenderer {
 public:
  void begin(const HoleMap* map, const View& view, uint16_t* dst, int stride, bool greenGrid);
  // Renders up to `rows` more rows (then the trees in one final step). True when the picture is complete.
  bool step(int rows);
  bool done() const { return phase_ == 2; }
  int percent() const;

 private:
  void renderRow(int py);
  void renderObjects();
  static constexpr int kLightMax = 96;              // light samples per row (every 4 px)
  const HoleMap* map_ = nullptr;
  View v_;
  uint16_t* dst_ = nullptr;
  int stride_ = 0;
  bool grid_ = false;
  int row_ = 0;
  int phase_ = 0;
  // per-shape pixel bounds (rows and columns) including the organic edge and a pixel of anti-aliasing
  int16_t top_[HoleMap::kMaxShapes], bottom_[HoleMap::kMaxShapes], left_[HoleMap::kMaxShapes],
      right_[HoleMap::kMaxShapes];
  int16_t gTop_, gBottom_, gLeft_, gRight_, tTop_, tBottom_, tLeft_, tRight_;
};

// Downsample (box filter) a byte-swapped RGB565 image (sw x sh, row stride `stride`) into dst (dw x dh).
void downsample(const uint16_t* src, int stride, int sw, int sh, uint16_t* dst, int dw, int dh);
// The whole-hole overview: the map shrunk to `h` rows (keeping its shape; the middle is cropped if it would be
// wider than maxW). Writes *outW and returns the overview's view.
View makeOverview(const uint16_t* map, const View& mapView, uint16_t* dst, int maxW, int h, int* outW);

}  // namespace golf
}  // namespace ui
