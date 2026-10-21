#pragma once
/**
 * Golf screens: everything the game shows, drawn straight into a 280x240 byte-swapped RGB565 buffer (the canvas's
 * memory) with alpha blending: the hole picture, ball and shadow, flag, aim line and landing ring, wind, HUD, the
 * golfer and power meter, banners, menus, scorecards. The app fills a Scene each frame; the host preview
 * (test/test_golf/render_host.cpp) draws the same Scenes to PNG.
 * Pure C++ (no Arduino).
 */
#include <stdint.h>

#include "core/golf.h"
#include "ui/golf_render.h"

namespace ui {
namespace golf {
using namespace core::golf;

constexpr int kScreenW = 280, kScreenH = 240, kTop = 38;   // below the system status bar
constexpr int kFeedbackX = 2, kFeedbackY = 82, kFeedbackW = 58, kFeedbackH = 104;    // the Shot Meter panel

enum class Screen : uint8_t { Title, CourseMenu, Intro, Play, Card, Calibrate };

struct Scene {
  Screen screen;
  // the picture: a hole map (scrolled) or an aux image (overview / green / title)
  const uint16_t* image;
  int imageW, imageH;
  View view;                 // world <-> image pixels
  int camX, camY;            // image pixel shown at screen (0, kTop)
  Theme theme;
  const HoleMap* map;         // for slope dots and range targets (may be null)
  // objects in the world
  bool showBall;
  float ballX, ballY, ballH;
  float pinX, pinY;
  bool bigFlag;
  int frame;                 // animation counter (flag wave, blinking)
  bool showAim;
  float aimDeg, aimYd;       // aim line from the ball; landing marker at aimYd
  float markerYd;            // where the live power puts the ball (< 0: hide)
  bool rangeTargets;         // label range target distances
  // slope arrows on the green (green view)
  bool showSlope;
  float slopeX, slopeY;
  // HUD
  bool hud;
  const char* courseName;
  int hole, par, yards, stroke;
  float toPin;
  int club;                  // index into core::golf::club()
  float clubYd;              // what the club carries at 100 % (putter: range)
  const char* lieName;
  float windMph, windDeg;
  // swing
  bool swinging;
  float livePower;           // 0..1.4 (unused when meterPhase is set)
  int golferFrame;           // 0..6
  // Wii-style meter: the target (backswing power), the fill (backswing, then forward-swing strength), red past the
  // bold 100 % line. meterPhase: 0 none, 1 backswing, 2 forward swing, 3 frozen at impact.
  uint8_t meterPhase;
  float meterFill, meterTarget;
  bool meterRed;
  const char* swingTag;      // e.g. "practice swing" over the golfer panel
  // the live curved aim line (the ground track the shot would fly), replaces the straight aim line when set
  const Point3* preview;
  int previewCount;
  uint8_t previewAlpha;      // 0 = 230
  bool previewWarn;          // tint it: this swing is heading the wrong way
  uint16_t previewColor;     // 0: white / previewWarn; else the shot shape's colour
  float distYd[4];           // Switch-Sports-style distance dots along the line (and on the meter), yards
  int distCount;
  // the Shot Meter and feedback bars (left column): live during the swing, frozen after impact (null: hidden)
  const SwingFeedback* feedback;
  float feedbackCarry;       // the club's full carry (labels the meter's distance dots)
  MeterBend meterBend;       // how the Shot Meter bends (Switch Sports): the swing's curve, and an over-swing's whip
  // optional cache of the drawn column (kFeedbackW x kFeedbackH, PSRAM): redrawn only when a value changes
  uint16_t* feedbackCache;
  uint32_t* feedbackKey;
  // hold A: how to play
  bool helpHint;             // show "hold A: how to play"
  bool helpOpen;
  int helpPage;
  // messages
  const char* banner;
  const char* sub;
  uint16_t bannerColor;      // RGB565
  const char* hint;          // small line at the bottom
  // menus
  const char* title;
  const char* const* items;
  int itemCount, selected;
  // scorecard
  const uint8_t* pars;
  const uint8_t* strokes;    // 0 = not played
  int holeCount;
  // feedback lines (range / after a swing), up to 4
  const char* lines[4];
  // overview card (intro)
  bool overview;
  int progress;              // 0..100, loading bar if < 100
};

void clearScene(Scene& s);
// Camera row for the play view: the ball near the bottom, and the point `aheadY` (the landing marker) in view.
int followCam(const View& v, int imageH, float ballY, float aheadY);
// Optional per-part timing (microseconds from `clock`): the picture copy, the world (flag, ball, aim, slope), the
// HUD and overlays.
struct DrawTiming {
  uint32_t (*clock)();
  uint32_t bgUs, worldUs, hudUs;
};
void drawScene(uint16_t* canvas, const Scene& s, DrawTiming* timing = nullptr);

// Primitives (screen pixels; clipped to y >= kTop). Colours are RGB565.
namespace gfx {
void blend(uint16_t* c, int x, int y, uint16_t col, uint8_t a);
void rect(uint16_t* c, int x, int y, int w, int h, uint16_t col, uint8_t a);
void panel(uint16_t* c, int x, int y, int w, int h, uint8_t a);
void disc(uint16_t* c, float cx, float cy, float rx, float ry, uint16_t col, uint8_t a);
void ring(uint16_t* c, float cx, float cy, float rx, float ry, float width, uint16_t col, uint8_t a);
void line(uint16_t* c, float x0, float y0, float x1, float y1, float width, uint16_t col, uint8_t a);
int textWidth(const char* s, int size);                      // size: 11, 13 or 25
void text(uint16_t* c, const char* s, int x, int baseline, int size, uint16_t col, int align = 0, bool shadow = false);
constexpr uint16_t rgb(unsigned r, unsigned g, unsigned b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }
}  // namespace gfx

}  // namespace golf
}  // namespace ui
