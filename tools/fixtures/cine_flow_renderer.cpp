// Host adapter for the actual Cine Scene builder and viewfinder raster.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <string>
#include <vector>
#include "ui/cine_camera_view.h"
#include "core/cine_controls.h"
#include "ui/list_row.h"
#include "ui/layout_metrics.h"
#include "ui/default_font_metrics.h"
#include "ui/safe_area_draw.h"
#include "ui/scrollbar_layout.h"
#include "ui/list_scroll_geometry.h"
#include <Fonts/Instrument/data.inc>
struct Canvas {
  std::vector<uint16_t> pixels = std::vector<uint16_t>(280 * 240, 0xffff);
  uint16_t at(int x, int y) const {
    return x >= 0 && y >= 0 && x < 280 && y < 240 ? pixels[y * 280 + x] : 0;
  }
  uint16_t readPixel(int x, int y) const { return at(x, y); }
  void fillRect(int x, int y, int w, int h, uint16_t color) {
    for (int yy = std::max(0, y); yy < std::min(240, y + h); ++yy)
      for (int xx = std::max(0, x); xx < std::min(280, x + w); ++xx)
        pixels[yy * 280 + xx] = color;
  }
  void pushImage(int x, int y, int w, int h, uint16_t *p) {
    for (int yy = 0; yy < h; ++yy)
      for (int xx = 0; xx < w; ++xx)
        fillRect(x + xx, y + yy, 1, 1, p[yy * w + xx]);
  }
  bool _vpOoB = false, inTransaction = false, lockTransaction = false;
  uint8_t textfont = 1, textsize = 1, datum = 0;
  uint16_t textcolor = 0, textbgcolor = 0xffff;
  int width() const { return 280; }
  int height() const { return 240; }
  void end_tft_write() {}
  void drawFastHLine(int x, int y, int w, uint32_t color) { fillRect(x, y, w, 1, color); }
  void drawPixel(int x, int y, uint16_t color) { fillRect(x, y, 1, 1, color); }
  void setTextFont(uint8_t f) { textfont = f; }
  void setTextSize(uint8_t size) { textsize = size; }
  void setTextWrap(bool, bool) {}
  void setTextColor(uint16_t fg) { textcolor = textbgcolor = fg; }
  void setTextColor(uint16_t fg, uint16_t bg) {
    textcolor = fg;
    textbgcolor = bg;
  }
  void setTextDatum(uint8_t value) { datum = value; }
  uint8_t getTextDatum() const { return datum; }
  int fontHeight(uint8_t = 0) const { return flash_font::height(textsize); }
  int textWidth(const char *value, uint8_t = 0) const {
    return ui::default_font::width(value, textsize);
  }
  int16_t drawString(const char *value, int x, int y, uint8_t = 0) {
    assert(datum == 0); // safe_area::paint resolves all alignment before this call.
    const int start = x;
    for (const unsigned char *ch = (const unsigned char *)value; *ch; ++ch)
      x += drawFlashFont(*ch, x, y, textcolor, textbgcolor, textsize);
    return x - start;
  }
  // Extracted verbatim from the pinned TFT library by render_cine_flow.py.
  // @TFT_FUNCTIONS@
  void text(const std::string &value, int x, int y, int size, uint16_t ink) {
    ui::instrument_text::text(*this, value.c_str(), x, y, instrument::fonts[size - 1], ink);
  }
  void save(const std::string &path) {
    FILE *f = fopen(path.c_str(), "wb");
    assert(f);
    fprintf(f, "P6\n280 240\n255\n");
    for (auto p : pixels) {
      uint8_t rgb[] = {uint8_t((p >> 11) * 255 / 31), uint8_t(((p >> 5) & 63) * 255 / 63),
                       uint8_t((p & 31) * 255 / 31)};
      fwrite(rgb, 1, 3, f);
    }
    fclose(f);
  }
};
// @STATUS_FUNCTIONS@
namespace ui {
namespace scene {
enum class TextAlign { TopLeft, TopCenter, MiddleCenter };
struct Box {
  int16_t x, y, w, h;
};
struct Node {
  Box rect;
  std::string text;
  uint16_t fg = 0, bg = 0;
  unsigned size = 1, radius = 0;
  bool label = false, bitmapTransparent = false, scrollOnFocus = false;
  TextAlign align = TextAlign::TopLeft;
};
struct Scene {
  std::vector<Node> nodes;
  void clear() { nodes.clear(); }
  Node *addRect(const char *, Box b, uint16_t color, int radius = 0, int = 0) {
    Node n;
    n.rect = b;
    n.bg = color;
    n.radius = radius;
    nodes.push_back(n);
    return &nodes.back();
  }
  Node *addBitmap(const char *, Box b, const uint8_t *, int, int, uint16_t fg, uint16_t bg, int = 0,
                  int = 1) {
    return addRect("", b, bg);
  }
  Node *addLabel(const char *, Box b, const char *text, uint16_t fg, uint16_t bg, int size = 1,
                 TextAlign align = TextAlign::TopLeft, int = 0) {
    Node n;
    n.rect = b;
    n.text = text;
    n.fg = fg;
    n.bg = bg;
    n.size = size;
    n.label = true;
    n.align = align;
    nodes.push_back(n);
    return &nodes.back();
  }
  void render(Canvas &c) {
    for (const auto &n : nodes) {
      if (n.label) {
        c.setTextFont(1);
        c.setTextSize(n.size);
        c.setTextColor(n.fg, n.bg);
        const uint8_t datum = n.align == TextAlign::TopCenter      ? TC_DATUM
                              : n.align == TextAlign::MiddleCenter ? MC_DATUM
                                                                   : TL_DATUM;
        safe_area::drawLabel(c, n.text.c_str(), {n.rect.x, n.rect.y, n.rect.w, n.rect.h}, datum, 1);
      } else if (n.radius)
        c.fillRoundRect(n.rect.x, n.rect.y, n.rect.w, n.rect.h, n.radius, n.bg);
      else
        c.fillRect(n.rect.x, n.rect.y, n.rect.w, n.rect.h, n.bg);
    }
  }
};
void setCurrentScene(Scene *) {}
} // namespace scene
} // namespace ui
namespace ui {
using ItemFn = ListItem (*)(int, void *);
struct ListView {
  int selected = 0, scrollFirst = 0, totalItems = 3, visibleRows = 0;
  int rowH = layout::kListRowH, rowPitch = layout::kListRowPitch, topPad = layout::kListTopPad,
      leftX = layout::kListLeftInset, scrollbarW = layout::kListScrollbarW,
      scrollbarPad = layout::kListScrollbarGap, scrollbarRight = layout::kListRightInset;
  void clampScroll();
  void buildScene(scene::Scene &, int, int, int, int, ItemFn, void *, uint16_t, uint16_t, uint16_t,
                  uint16_t, uint16_t);
};
// @LIST_FUNCTIONS@
} // namespace ui
namespace cine {
struct Session {
  struct Shutter {
    bool valid = true;
    uint32_t actualUs = 16667;
  } s;
  struct Status {
    const char *message = "Camera resources busy";
  } st;
  unsigned color = 0;
  bool automatic = true;
  const Shutter &shutter() const { return s; }
  bool autoExposure() const { return automatic; }
  const Status &status() const { return st; }
  unsigned colorMode() const { return color; }
  static const char *colorName(unsigned w) {
    const char *names[] = {"Sunny", "Gold", "Portra", "Mono", "Cool", "Vivid"};
    return names[w];
  }
};
} // namespace cine
namespace services { namespace camera {
enum class CaptureResolution { Vga, Hd };
enum class Setting { Capture };
} }
struct { services::camera::CaptureResolution capture=services::camera::CaptureResolution::Vga;
  void save(services::camera::Setting) {} } cameraSettings;
bool microphone=true, clipActive=false, tutorialCapture=false;
const char* clipCaption(){return nullptr;}
cine::Session session;
core::cine::WheelControl wheel;
ui::ListView settingsList;
bool menuEditing = false;
enum class View { Menu, Opening, Preview, Error };
View current = View::Preview;
unsigned feedbackStage = 0;
constexpr uint16_t bg = 0xffff, fg = 0, muted = 0x632c;
uint32_t millis() { return 100; }
// @APP_FUNCTIONS@
int main(int argc, char **argv) {
  assert(argc == 3 || argc == 4);
  const uint32_t reviewShutter = argc == 4 ? std::stoul(argv[3]) : 16667;
  std::string out = argv[2];
  std::vector<std::vector<uint16_t>> photos(6, std::vector<uint16_t>(224 * 168));
  for (unsigned mode = 0; mode < photos.size(); ++mode) {
    const auto path = std::string(argv[1]) + std::to_string(mode) + ".rgb565";
    FILE *f = fopen(path.c_str(), "rb");
    assert(f && fread(photos[mode].data(), 2, photos[mode].size(), f) == photos[mode].size());
    fclose(f);
  }
  struct Frame {
    const char *id;
    unsigned color, stage;
    bool controls, colorMode;
    uint32_t shutter;
  };
  const Frame frames[] = {{"live", 0, 0, false, false, reviewShutter},
                          {"shutter", 0, 0, true, false, reviewShutter},
                          {"shutter-auto", 0, 0, true, false, reviewShutter},
                          {"shutter-mono", 3, 0, true, false, reviewShutter},
                          {"color-sunny", 0, 0, true, true, reviewShutter},
                          {"color-gold", 1, 0, true, true, reviewShutter},
                          {"color-portra", 2, 0, true, true, reviewShutter},
                          {"color-mono", 3, 0, true, true, reviewShutter},
                          {"color-cool", 4, 0, true, true, reviewShutter},
                          {"color-vivid", 5, 0, true, true, reviewShutter},
                          {"capture", 0, 1, false, false, reviewShutter},
                          {"saved", 0, 5, false, false, reviewShutter},
                          {"save-failed", 0, 6, false, false, reviewShutter}};
  for (const auto &s : frames) {
    Canvas c;
    ui::cine_view::image(c, photos[s.color].data(), 224, 168, s.stage == 1);
    session.s.actualUs = s.shutter;
    session.automatic = !s.controls || s.colorMode || std::string(s.id) == "shutter-auto";
    char shutter[20];
    shutterValue(shutter, sizeof(shutter));
    ui::cine_view::chrome(c, cine::Session::colorName(s.color), shutter, s.stage * 1000 + 100,
                          s.colorMode, s.controls);
    c.save(out + "/" + s.id + ".ppm");
  }
  for (int selected = 0; selected < 3; ++selected)
    for (int editing = 0; editing < (selected == 2 ? 1 : 2); ++editing) {
      current = View::Menu;
      session.automatic = !(selected == 0 && editing);
      settingsList.selected = selected;
      menuEditing = editing;
      Canvas c;
      ui::scene::Scene scene;
      build(scene, 280, 240);
      scene.render(c);
      ui::drawBarGlyphs(c, 280, 38, 4, 99, false, true, false, "--:--", 0, false, false, false, 0, 0, 0,
                        0xffff);
      std::string id = selected == 0   ? "settings-ss"
                       : selected == 1 ? "settings-color"
                                       : "settings-back";
      if (editing)
        id += "-edit";
      c.save(out + "/" + id + ".ppm");
    }
  for (View state : {View::Opening, View::Error}) {
    current = state;
    Canvas c;
    ui::scene::Scene scene;
    build(scene, 280, 240);
    scene.render(c);
    ui::drawBarGlyphs(c, 280, 38, 4, 99, false, true, false, "--:--", 0, false, false, false, 0, 0, 0,
                      0xffff);
    c.save(out + (state == View::Opening ? "/preparing.ppm" : "/error.ppm"));
  }
  Canvas splash;
  const int mid = 38 + (240 - 38) / 2;
  auto centered = [&](const char *s, int y, int size, uint16_t ink) {
    splash.text(s, (280 - ui::instrument_text::width(s, instrument::fonts[size - 1])) / 2,
                y - instrument::fonts[size - 1].height / 2, size, ink);
  };
  centered("Opening", mid - 36, 1, kUiMuted);
  centered("Camera", mid - 6, 3, kUiFg);
  centered("...", mid + 28, 1, kUiAccent);
  ui::drawBarGlyphs(splash, 280, 38, 4, 99, false, true, false, "--:--", 0, false, false, false, 0, 0, 0,
                    0xffff);
  splash.save(out + "/opening.ppm");
}
