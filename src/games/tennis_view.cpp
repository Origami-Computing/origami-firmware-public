// Tennis drawing, retained: see include/games/tennis_view.h.
#include "games/tennis_view.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include <initializer_list>

#include "assets/tennis_art.h"
#include "ui/tennis_raster.h"
#include "games/tennis_help.h"

namespace games {
namespace tennis_view {

namespace A = tennis_art;
namespace T = core::tennis;
namespace R = ui::tennis::raster;
namespace D = games::dirty;
namespace H = games::tennis_help;

namespace {

constexpr int kW = 280, kH = 240, kTop = 38;
static_assert(sizeof(tennis_art::kBackground) == kW * kH * sizeof(uint16_t),
              "kBackground must be exactly one 280x240 RGB565 canvas");
constexpr uint16_t rgb(unsigned r, unsigned g, unsigned b) { return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3); }
constexpr uint16_t kPanel = rgb(20, 32, 48), kShadow = 0x18C3;
constexpr int kHintRight = 274, kHintY = 42;     // the "hold A" hint, top right

// ---- camera (the same projection the art generator used) ----
float depthScale(float z) { return 1.0f / (1.0f + A::kDepthK * z); }
float groundY(float z) {
  const float far = depthScale(1.0f);
  return A::kTopY + (A::kBottomY - A::kTopY) * (depthScale(z) - far) / (1.0f - far);
}
int sx(float x, float z) { return (int)lroundf(A::kCx + x * A::kHalfW * depthScale(z)); }
int sy(float z, float h) { return (int)lroundf(groundY(z) - h * A::kHeightPx * depthScale(z)); }

Rect clipToScreen(Rect r) {
  if (r.x0 < 0) r.x0 = 0;
  if (r.y0 < kTop) r.y0 = kTop;
  if (r.x1 > kW) r.x1 = kW;
  if (r.y1 > kH) r.y1 = kH;
  if (D::empty(r)) r = Rect{0, 0, 0, 0};
  return r;
}
Rect uniteOrFirst(const Rect& a, const Rect& b) {
  if (D::empty(a)) return b;
  if (D::empty(b)) return a;
  return D::unite(a, b);
}

// ---- primitives ----
struct Ellipse {
  float cx, cy, rx, ry;
  uint16_t col;
  uint8_t alpha;
};
Rect box(const Ellipse& e) {
  return Rect{(int16_t)floorf(e.cx - e.rx - 1), (int16_t)floorf(e.cy - e.ry - 1), (int16_t)(ceilf(e.cx + e.rx + 1) + 1),
              (int16_t)(ceilf(e.cy + e.ry + 1) + 1)};
}
void hash(D::Hash& h, const Ellipse& e) { h.f(e.cx).f(e.cy).f(e.rx).f(e.ry).u(e.col).u(e.alpha); }

// Filled ellipse with a soft (alpha) edge; a pixel's coverage depends only on its position.
void paint(const R::Canvas& c, const Ellipse& e) {
  const Rect b = box(e);
  const int x0 = b.x0 < c.x0 ? c.x0 : b.x0, x1 = b.x1 > c.x1 ? c.x1 : b.x1;
  const int y0 = b.y0 < c.y0 ? c.y0 : b.y0, y1 = b.y1 > c.y1 ? c.y1 : b.y1;
  const float k = fminf(e.rx, e.ry);
  for (int y = y0; y < y1; ++y) {
    const float dy = (y + 0.5f - e.cy) / e.ry;
    uint16_t* row = c.px + y * c.w;
    for (int x = x0; x < x1; ++x) {
      const float dx = (x + 0.5f - e.cx) / e.rx;
      const float cover = fminf(1.0f, fmaxf(0.0f, (1.0f - sqrtf(dx * dx + dy * dy)) * k + 0.5f));
      if (cover > 0) {
        const uint8_t a = (uint8_t)(cover * e.alpha);
        if (a) R::blendInto(row[x], e.col, a);
      }
    }
  }
}

// Ink box of a text line (the glyphs' own rectangles), as R::text() lays it out.
Rect textBox(const char* t, int x, int base, const doodle_art::Font& f, int align) {
  const int w = R::textWidth(t, f);
  if (align == 1) x -= w / 2;
  else if (align == 2) x -= w;
  Rect r{0, 0, 0, 0};
  for (; *t; ++t) {
    const unsigned ch = (unsigned char)*t;
    if (ch < 32 || ch >= 127) continue;
    const doodle_art::Glyph& g = f.glyphs[ch - 32];
    if (g.w && g.h)
      r = uniteOrFirst(r, Rect{(int16_t)(x + g.x), (int16_t)(base + g.y), (int16_t)(x + g.x + g.w), (int16_t)(base + g.y + g.h)});
    x += g.advance;
  }
  return r;
}

// ---- items ----
enum Item : int { kBall, kAi, kPlayer, kScore, kMessage, kHint, kHelp, kItemCount };
static_assert(kItemCount <= Retained::kItems, "items");

// Racket frame: 0 = right side .. 6 = left side; a swing plays through them over 220 ms.
int racketFrame(T::Side ready, uint32_t now, uint32_t swingAt, T::Side swingSide) {
  const uint32_t since = now - swingAt;
  if (swingAt && since < 220 && swingSide != T::Side::None) {
    const int f = (int)(since * 7 / 220);
    return swingSide == T::Side::Forehand ? f : 6 - f;
  }
  return ready == T::Side::Backhand ? 6 : 0;
}

struct Figure {
  const doodle_art::Sprite* img;
  int x, y;                 // sprite top-left
  Ellipse shadow;
};
Figure figure(const doodle_art::Sprite* frames, int ax, int ay, float x, float z, int frame) {
  const int fx = sx(x, z), fy = sy(z, 0);
  Figure f;
  f.img = &frames[frame];
  f.x = fx - ax;
  f.y = fy - ay;
  f.shadow = Ellipse{(float)fx, (float)fy, 16.0f * depthScale(z) + 3, 4.5f * depthScale(z) + 1, kShadow, 110};
  return f;
}

enum Slot : uint8_t { kFar, kMiddle, kNear };   // ball behind the AI / between the players / in front of the player
struct Ball {
  bool shown;
  uint8_t slot;
  Ellipse e[4];             // shadow, outline, ball, highlight
};
Ball ball(const T::State& st) {
  Ball r;
  memset(&r, 0, sizeof(r));
  if (st.phase == T::Phase::PointOver || st.phase == T::Phase::MatchOver) return r;
  const T::Ball& b = st.ball;
  const float k = depthScale(b.z), rad = 3.6f * k + 1.2f;
  const float bx = A::kCx + b.x * A::kHalfW * k;
  const float by = groundY(b.z) - b.h * A::kHeightPx * k;
  r.shown = true;
  r.slot = b.z > 1.02f ? kFar : ((b.z < -0.04f && st.phase != T::Phase::Toss) ? kNear : kMiddle);
  r.e[0] = Ellipse{bx, groundY(b.z), rad * 1.3f, rad * 0.55f, kShadow, 120};
  r.e[1] = Ellipse{bx, by, rad + 0.8f, rad + 0.8f, rgb(70, 90, 20), 255};
  r.e[2] = Ellipse{bx, by, rad, rad, rgb(222, 244, 60), 255};
  r.e[3] = Ellipse{bx - rad * 0.3f, by - rad * 0.35f, rad * 0.35f, rad * 0.35f, rgb(250, 255, 200), 200};
  return r;
}

struct Hud {
  char you[8], ai[8], youGames[6], aiGames[6];
  const char* msg;
  int msgW;                 // message panel width
  const char* hint;
  int hintW;
  bool help;
  uint8_t page;
};

// Everything one frame shows, with each item's box and hash.
struct Frame {
  Figure ai, player;
  Ball ball;
  Hud hud;
  Rect box[kItemCount];
  uint32_t sig[kItemCount];
};

void build(Frame& f, const T::Game& g, const Scene& s) {
  const T::State& st = g.state();
  f.ai = figure(A::kAi, A::kAiAnchorX, A::kAiAnchorY, st.ai.x, 1.04f,
                racketFrame(st.ball.x > st.ai.x ? T::Side::Forehand : T::Side::Backhand, s.nowMs, s.aiSwingAt,
                            s.aiSwingSide));
  f.player = figure(A::kPlayer, A::kPlayerAnchorX, A::kPlayerAnchorY, st.player.x, -0.04f,
                    racketFrame(st.phase == T::Phase::Rally ? g.sideNeeded() : T::Side::Forehand, s.nowMs, s.swingAt,
                                s.swingSide));
  f.ball = ball(st);
  T::Game::scoreText(st.playerPoints, st.aiPoints, f.hud.you, sizeof(f.hud.you));
  T::Game::scoreText(st.aiPoints, st.playerPoints, f.hud.ai, sizeof(f.hud.ai));
  snprintf(f.hud.youGames, sizeof(f.hud.youGames), "%u", st.playerGames);
  snprintf(f.hud.aiGames, sizeof(f.hud.aiGames), "%u", st.aiGames);
  f.hud.msg = s.message && *s.message ? s.message : nullptr;
  f.hud.msgW = f.hud.msg ? R::textWidth(f.hud.msg, doodle_art::font16) + 20 : 0;
  f.hud.hint = s.hint && *s.hint ? s.hint : nullptr;
  f.hud.hintW = f.hud.hint ? R::textWidth(f.hud.hint, H::footerFont()) + 14 : 0;
  f.hud.help = s.helpOpen;
  f.hud.page = s.helpPage < H::kPageCount ? s.helpPage : H::kPageCount - 1;

  const Figure* figs[2] = {&f.ai, &f.player};
  for (int i = 0; i < 2; ++i) {
    const Figure& fg = *figs[i];
    const Rect spr{(int16_t)fg.x, (int16_t)fg.y, (int16_t)(fg.x + fg.img->w), (int16_t)(fg.y + fg.img->h)};
    f.box[i ? kPlayer : kAi] = clipToScreen(D::unite(spr, box(fg.shadow)));
    D::Hash h(i ? kPlayer : kAi);
    h.p(fg.img).u((uint32_t)fg.x).u((uint32_t)fg.y);
    hash(h, fg.shadow);
    f.sig[i ? kPlayer : kAi] = h.v;
  }
  {
    D::Hash h(kBall);
    Rect b{0, 0, 0, 0};
    if (f.ball.shown) {
      h.u(f.ball.slot);
      for (const Ellipse& e : f.ball.e) {
        b = uniteOrFirst(b, box(e));
        hash(h, e);
      }
    }
    f.box[kBall] = clipToScreen(b);
    f.sig[kBall] = h.v;
  }
  {
    Rect b{6, 42, 110, 80};
    b = uniteOrFirst(b, textBox("YOU", 22, 58, doodle_art::font13, 0));
    b = uniteOrFirst(b, textBox("CPU", 22, 73, doodle_art::font13, 0));
    b = uniteOrFirst(b, textBox(f.hud.youGames, 62, 58, doodle_art::font13, 1));
    b = uniteOrFirst(b, textBox(f.hud.aiGames, 62, 73, doodle_art::font13, 1));
    b = uniteOrFirst(b, textBox(f.hud.you, 102, 58, doodle_art::font13, 2));
    b = uniteOrFirst(b, textBox(f.hud.ai, 102, 73, doodle_art::font13, 2));
    f.box[kScore] = clipToScreen(b);
    f.sig[kScore] = D::Hash(kScore).s(f.hud.you).s(f.hud.ai).s(f.hud.youGames).s(f.hud.aiGames).v;
  }
  {
    Rect b{0, 0, 0, 0};
    if (f.hud.msg) {
      b = Rect{(int16_t)(kW / 2 - f.hud.msgW / 2 + 30), 88, (int16_t)(kW / 2 - f.hud.msgW / 2 + 30 + f.hud.msgW), 112};
      b = uniteOrFirst(b, textBox(f.hud.msg, kW / 2 + 30, 105, doodle_art::font16, 1));
    }
    f.box[kMessage] = clipToScreen(b);
    f.sig[kMessage] = D::Hash(kMessage).s(f.hud.msg).v;
  }
  {
    Rect b{0, 0, 0, 0};
    if (f.hud.hint) {
      b = Rect{(int16_t)(kHintRight - f.hud.hintW), kHintY, kHintRight, kHintY + 19};
      b = uniteOrFirst(b, textBox(f.hud.hint, kHintRight - f.hud.hintW / 2, kHintY + 14, H::footerFont(), 1));
    }
    f.box[kHint] = clipToScreen(b);
    f.sig[kHint] = D::Hash(kHint).s(f.hud.hint).v;
  }
  f.box[kHelp] = f.hud.help ? clipToScreen(Rect{H::kCardX, H::kCardY, H::kCardX + H::kCardW, H::kCardY + H::kCardH})
                            : Rect{0, 0, 0, 0};
  f.sig[kHelp] = D::Hash(kHelp).u(f.hud.help).u(f.hud.page).v;
}

void paintFigure(const R::Canvas& c, const Figure& f) {
  paint(c, f.shadow);
  R::blit(c, *f.img, f.x, f.y);
}
void paintBall(const R::Canvas& c, const Ball& b) {
  for (const Ellipse& e : b.e) paint(c, e);
}

// The picture in depth order, clipped to c; items outside the clip are skipped.
void paintSprites(const R::Canvas& c, const Frame& f) {
  const Rect clip{(int16_t)c.x0, (int16_t)c.y0, (int16_t)c.x1, (int16_t)c.y1};
  const bool ballHere = f.ball.shown && D::overlap(f.box[kBall], clip);
  if (ballHere && f.ball.slot == kFar) paintBall(c, f.ball);
  if (D::overlap(f.box[kAi], clip)) paintFigure(c, f.ai);
  if (ballHere && f.ball.slot == kMiddle) paintBall(c, f.ball);
  if (D::overlap(f.box[kPlayer], clip)) paintFigure(c, f.player);
  if (ballHere && f.ball.slot == kNear) paintBall(c, f.ball);
}
// The how-to-play card: Wii-style translucent rounded card, title, page heading and text, page dots, footer.
void paintHelp(const R::Canvas& c, const Frame& f) {
  const H::Page& pg = H::kPages[f.hud.page];
  R::panel(c, H::kCardX, H::kCardY, H::kCardW, H::kCardH, rgb(200, 220, 255), 110, 10);            // rim
  R::panel(c, H::kCardX + 2, H::kCardY + 2, H::kCardW - 4, H::kCardH - 4, rgb(16, 26, 44), 225, 8);
  R::text(c, H::kTitle, H::kTextX, H::kCardY + 25, H::titleFont(), rgb(255, 222, 110));
  char num[8];
  snprintf(num, sizeof(num), "%d/%d", f.hud.page + 1, H::kPageCount);
  R::text(c, num, H::kTextX + H::kTextW, H::kCardY + 24, H::footerFont(), rgb(170, 185, 210), 2);
  R::fillRect(c, H::kTextX, H::kCardY + 33, H::kTextW, 1, rgb(90, 110, 150));
  R::text(c, pg.heading, H::kTextX, H::kCardY + 53, H::headingFont(), rgb(130, 200, 255));
  for (int k = 0; k < H::kLines && pg.lines[k]; ++k)
    R::text(c, pg.lines[k], H::kTextX, H::kCardY + 72 + 16 * k, H::bodyFont(), 0xFFFF);
  const float dotsX = H::kCardX + H::kCardW / 2.0f - (H::kPageCount - 1) * 6.0f;
  for (int i = 0; i < H::kPageCount; ++i)
    paint(c, Ellipse{dotsX + 12.0f * i, (float)(H::kCardY + 166), 2.6f, 2.6f,
                     i == f.hud.page ? rgb(255, 222, 110) : rgb(90, 110, 150), 255});
  R::text(c, H::kFooter, H::kCardX + H::kCardW / 2, H::kCardY + 185, H::footerFont(), rgb(170, 185, 210), 1);
}

void paintHud(const R::Canvas& c, const Frame& f) {
  const Rect clip{(int16_t)c.x0, (int16_t)c.y0, (int16_t)c.x1, (int16_t)c.y1};
  if (D::overlap(f.box[kScore], clip)) {
    R::panel(c, 6, 42, 104, 38, kPanel, 190);
    R::fillRect(c, 12, 49, 5, 9, rgb(232, 74, 60));
    R::fillRect(c, 12, 64, 5, 9, rgb(58, 123, 213));
    R::text(c, "YOU", 22, 58, doodle_art::font13, 0xFFFF);
    R::text(c, "CPU", 22, 73, doodle_art::font13, 0xFFFF);
    R::text(c, f.hud.youGames, 62, 58, doodle_art::font13, rgb(255, 214, 90), 1);
    R::text(c, f.hud.aiGames, 62, 73, doodle_art::font13, rgb(255, 214, 90), 1);
    R::text(c, f.hud.you, 102, 58, doodle_art::font13, 0xFFFF, 2);
    R::text(c, f.hud.ai, 102, 73, doodle_art::font13, 0xFFFF, 2);
  }
  if (f.hud.msg && D::overlap(f.box[kMessage], clip)) {
    R::panel(c, kW / 2 - f.hud.msgW / 2 + 30, 88, f.hud.msgW, 24, kPanel, 200);
    R::text(c, f.hud.msg, kW / 2 + 30, 105, doodle_art::font16, 0xFFFF, 1);
  }
  if (f.hud.hint && D::overlap(f.box[kHint], clip)) {
    R::panel(c, kHintRight - f.hud.hintW, kHintY, f.hud.hintW, 19, kPanel, 170);
    R::text(c, f.hud.hint, kHintRight - f.hud.hintW / 2, kHintY + 14, H::footerFont(), rgb(255, 230, 150), 1);
  }
  if (f.hud.help && D::overlap(f.box[kHelp], clip)) paintHelp(c, f);
}

}  // namespace

void invalidate(Retained& r) { r.valid = false; }

void draw(uint16_t* canvas, const T::Game& g, const Scene& s, Retained* ret, Timing* tm) {
  const uint32_t t0 = tm ? tm->clockUs() : 0;
  Frame f;
  build(f, g, s);
  const bool full = !ret || !ret->valid || ret->canvas != canvas ||
                    (int32_t)(s.nowMs - ret->fullAt) >= (int32_t)Retained::kFullMs;
  Rect rects[Retained::kMaxRects + 1];
  int n = 0;
  if (full) {
    rects[n++] = Rect{0, kTop, kW, kH};
  } else {
    for (int i = 0; i < kItemCount; ++i) {
      if (f.sig[i] == ret->sig[i] && D::same(f.box[i], ret->box[i])) continue;
      for (const Rect* r : {&ret->box[i], &f.box[i]}) {
        if (D::empty(*r)) continue;
        rects[n++] = *r;
        if (n > Retained::kMaxRects) n = D::mergeRects(rects, n, Retained::kMaxRects);
      }
    }
    n = D::mergeRects(rects, n, Retained::kMaxRects);
  }
  const uint32_t t1 = tm ? tm->clockUs() : 0;
  uint32_t px = 0;
  for (int k = 0; k < n; ++k) {                                  // the background back where things changed
    const Rect& r = rects[k];
    const size_t bytes = (size_t)(r.x1 - r.x0) * 2;
    for (int y = r.y0; y < r.y1; ++y) memcpy(canvas + y * kW + r.x0, A::kBackground + y * kW + r.x0, bytes);
    px += (uint32_t)D::area(r);
  }
  const uint32_t t2 = tm ? tm->clockUs() : 0;
  uint32_t spritesUs = 0, hudUs = 0;
  for (int k = 0; k < n; ++k) {                                  // repaint, clipped to each rectangle
    const R::Canvas c{canvas, kW, kH, rects[k].x0, rects[k].y0, rects[k].x1, rects[k].y1};
    const uint32_t a = tm ? tm->clockUs() : 0;
    paintSprites(c, f);
    const uint32_t b = tm ? tm->clockUs() : 0;
    paintHud(c, f);
    const uint32_t e = tm ? tm->clockUs() : 0;
    spritesUs += b - a;
    hudUs += e - b;
  }
  if (ret) {
    for (int i = 0; i < kItemCount; ++i) {
      ret->box[i] = f.box[i];
      ret->sig[i] = f.sig[i];
    }
    ret->canvas = canvas;
    if (full) ret->fullAt = s.nowMs;
    ret->valid = true;
    ret->rects = (uint8_t)n;
    for (int k = 0; k < n; ++k) ret->rect[k] = rects[k];
  }
  if (tm) {
    uint32_t spritePx = 0, hudPx = 0;
    uint16_t rows = 0;
    for (int y = kTop; y < kH; ++y)
      for (int k = 0; k < n; ++k)
        if (y >= rects[k].y0 && y < rects[k].y1) { ++rows; break; }
    for (int k = 0; k < n; ++k)
      for (int i = 0; i < kItemCount; ++i) {
        if (i == kBall && !f.ball.shown) continue;
        const Rect& b = f.box[i];
        const int w = (b.x1 < rects[k].x1 ? b.x1 : rects[k].x1) - (b.x0 > rects[k].x0 ? b.x0 : rects[k].x0);
        const int h = (b.y1 < rects[k].y1 ? b.y1 : rects[k].y1) - (b.y0 > rects[k].y0 ? b.y0 : rects[k].y0);
        if (w > 0 && h > 0) (i >= kScore ? hudPx : spritePx) += (uint32_t)(w * h);
      }
    tm->spritePx = spritePx;
    tm->hudPx = hudPx;
    tm->rows = rows;
    tm->measureUs = t1 - t0;
    tm->bgUs = t2 - t1;
    tm->spritesUs = spritesUs;
    tm->hudUs = hudUs;
    tm->area = px;
    tm->rects = (uint8_t)n;
  }
}

}  // namespace tennis_view
}  // namespace games
