/**
 * @file remote_display_app.cpp
 * @brief Remote Display — Wi-Fi framebuffer receiver for desktop mirror/extend experiments.
 *
 * The app is intentionally a direct-panel screen. The incoming frame stream is
 * already display-shaped, so the normal landscape canvas + transpose pipeline
 * would add latency and extra SPI traffic without improving composition.
 */

#include "apps/remote_display_app.h"
#include "ui/safe_area_draw.h"

#include <Arduino.h>

#include "app.h"
#include "config/ui_theme.h"
#include "services/remote_display_stream.h"
#include "hal/hal_display.h"
#include "hal/hal_imu.h"
#include "hal/hal_input.h"
#include "hal/hal_wifi.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "ui/nav_controls.h"

#include <JPEGDecoder.h>

namespace apps {
namespace remote_display {

namespace rd = services::remote_display;

static bool s_active = false;
static uint32_t s_lastStatusMs = 0;
static uint32_t s_lastDrawnFrameId = 0;
static uint32_t s_lastFpsMs = 0;
static uint32_t s_lastFpsFrames = 0;
static uint32_t s_lastDeviceTapMs = 0;
static uint16_t s_estFps10 = 0;

static void drawStatus(bool force) {
  const uint32_t now = millis();
  if (!force && (uint32_t)(now - s_lastStatusMs) < 500) return;
  s_lastStatusMs = now;

  const rd::Stats st = rd::stats();
  TFT_eSPI& d = hal::display::directTft();
  hal::display::beginDirectDraw();
  d.fillRect(0, 0, d.width(), d.height(), kUiBg);
  d.setTextDatum(TL_DATUM);
  d.setTextFont(1);
  d.setTextSize(1);
  d.setTextColor(kUiFg, kUiBg);
  ui::safe_area::drawString(d, "Remote Display", 12, ui::safe_area::contentTop()+6);
  d.setTextColor(kUiMuted, kUiBg);
  ui::safe_area::drawString(d, hal::wifi::connected() ? "Wi-Fi ready" : "Waiting for Wi-Fi", 12, 60);

  char line[48];
  snprintf(line, sizeof(line), "IP: %s", rd::localIpString());
  ui::safe_area::drawString(d, line, 12, 78);
  snprintf(line, sizeof(line), "Port: %u", (unsigned)st.port);
  ui::safe_area::drawString(d, line, 12, 96);
  snprintf(line, sizeof(line), "Host: %s", st.connected ? "connected" : "not connected");
  ui::safe_area::drawString(d, line, 12, 114);
  snprintf(line, sizeof(line), "Frames: %lu drop %lu",
           (unsigned long)st.framesReceived,
           (unsigned long)st.framesDropped);
  ui::safe_area::drawString(d, line, 12, 132);
  ui::safe_area::drawString(d, "LB or double tap: exit", 12, d.height() - 24);
  hal::display::endDirectDraw();
}

static void drawOverlay(const rd::Stats& st) {
  TFT_eSPI& d = hal::display::directTft();
  char line[56];
  snprintf(line, sizeof(line), "%ux%u  %u.%u fps  drop %lu",
           (unsigned)st.sourceW, (unsigned)st.sourceH,
           (unsigned)(s_estFps10 / 10), (unsigned)(s_estFps10 % 10),
           (unsigned long)st.framesDropped);

  hal::display::beginDirectDraw();
  d.fillRect(0, d.height() - 14, d.width(), 14, TFT_BLACK);
  d.setTextDatum(TL_DATUM);
  d.setTextFont(1);
  d.setTextSize(1);
  d.setTextColor(TFT_WHITE, TFT_BLACK);
  ui::safe_area::drawString(d, line, 3, d.height() - 12);
  hal::display::endDirectDraw();
}

static bool drawJpegFrame(const rd::Frame& f) {
  if (!f.data || f.len == 0) return false;
  if (!JpegDec.decodeArray(const_cast<uint8_t*>(f.data), f.len)) return false;

  TFT_eSPI& d = hal::display::directTft();
  const uint16_t mcuW = JpegDec.MCUWidth;
  const uint16_t mcuH = JpegDec.MCUHeight;

  hal::display::beginDirectDraw();
  d.startWrite();
  while (JpegDec.readSwappedBytes()) {
    uint16_t* pImg = (uint16_t*)JpegDec.pImage;
    const int dx = (int)JpegDec.MCUx * (int)mcuW;
    const int dy = (int)JpegDec.MCUy * (int)mcuH;
    if (dx >= d.width() || dy >= d.height()) continue;
    const int w = (dx + mcuW > d.width()) ? d.width() - dx : mcuW;
    const int h = (dy + mcuH > d.height()) ? d.height() - dy : mcuH;
    if (w == (int)mcuW && h == (int)mcuH) {
      d.pushImage(dx, dy, mcuW, mcuH, pImg);
    } else {
      for (int row = 0; row < h; row++) {
        d.pushImage(dx, dy + row, w, 1, pImg + row * mcuW);
      }
    }
  }
  d.endWrite();
  hal::display::endDirectDraw();
  return true;
}

static void startSession() {
  s_active = true;
  rd::start();
}

static void enterView() {
  s_lastStatusMs = 0;
  s_lastDrawnFrameId = 0;
  s_lastFpsMs = millis();
  s_lastFpsFrames = 0;
  s_lastDeviceTapMs = 0;
  s_estFps10 = 0;
  hal::display::enterOptimizedRender();
  hal::display::fixColorInversion();
  drawStatus(true);
}

static void exitApp() {
  s_active = false;
  rd::stop();
  hal::display::exitOptimizedRender();
}

static void onTick(uint32_t now) {
  if (!s_active) return;
  if (hal::imu::isReady() && hal::imu::pollTap(now)) {
    if (s_lastDeviceTapMs != 0 && (uint32_t)(now - s_lastDeviceTapMs) <= 700u) {
      NavHandler::instance().pop();
      return;
    }
    s_lastDeviceTapMs = now;
  }

  rd::service(now);

  rd::Stats st = rd::stats();
  if (rd::frameReady()) {
    const rd::Frame f = rd::frame();
    if (drawJpegFrame(f)) {
      s_lastDrawnFrameId = f.frameId;
    }
    rd::consumeFrame();
    st = rd::stats();

    if ((uint32_t)(now - s_lastFpsMs) >= 1000) {
      const uint32_t deltaFrames = st.framesReceived - s_lastFpsFrames;
      const uint32_t deltaMs = now - s_lastFpsMs;
      s_estFps10 = (uint16_t)((deltaFrames * 10000u) / (deltaMs ? deltaMs : 1u));
      s_lastFpsFrames = st.framesReceived;
      s_lastFpsMs = now;
    }
    drawOverlay(st);
    return;
  }

  if (!st.connected || s_lastDrawnFrameId == 0) {
    drawStatus(false);
  }
}

static void onInput(hal::input::InputEvent ev) {
  if (ui::nav::isBackTap(ev)) {
    NavHandler::instance().pop();
  }
}

static bool onChord() {
  return true;
}

bool isActive() {
  return s_active;
}

const ::scene::Screen kScreenRemoteDisplay = {
    "Remote Display",
    enterView,
    nullptr,
    onTick,
    onInput,
    onChord,
    nullptr,
    "remote-display",
    nullptr,
};

static const ::scene::Screen* const kScreens[] = {&kScreenRemoteDisplay};
static core::health::RuntimePolicy runtimePolicy() {
    return core::health::whileBusy(isActive());
}
App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Remote Display", "remote-display",
                            &kScreenRemoteDisplay, startSession, startSession, exitApp,
                            "src/apps/remote_display_app.cpp", kScreens, nullptr, runtimePolicy);

}  // namespace remote_display
}  // namespace apps
