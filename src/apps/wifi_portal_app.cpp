#include "config/screen_chrome_layout.h"
/**
 * @file wifi_portal_app.cpp
 * @brief Soft-AP + captive-portal WiFi provisioning (scene-rendered).
 *
 * The app owns the provisioning view and navigation. services::wifi_portal
 * owns the AP/webserver session and delegates radio restoration to the HAL.
 */

#include "apps/wifi_portal_app.h"

#include <Arduino.h>

#include "app.h"
#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "config/wifi_portal_layout.h"
#include "hal/hal_display.h"
#include "services/wifi_portal.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "ui/nav_controls.h"
#include "ui/scene.h"

namespace apps {
namespace wifi_portal {

static bool     s_needRedraw       = false;
static uint32_t s_lastStatusPollMs = 0;
static int      s_lastClientCount  = -1;
static bool     s_lastStaConnected = false;

// ─── App vtable callbacks ────────────────────────────────────────────────────

static void beginSession() {
  Serial.println("[wifi-portal] entering");
  services::wifi_portal::open();
}

void enter() {
  s_needRedraw       = true;
  s_lastStatusPollMs = 0;
  s_lastClientCount  = -1;
  s_lastStaConnected = false;
  hal::display::markAllDirty();
}

void exitApp() {
  Serial.println("[wifi-portal] exiting");
  services::wifi_portal::close();
}

void tick() {
  services::wifi_portal::poll();

  const uint32_t now = millis();
  if ((uint32_t)(now - s_lastStatusPollMs) < 1500u) return;
  s_lastStatusPollMs = now;

  const auto status = services::wifi_portal::status();
  const int clients = status.clients;
  const bool sta = status.stationConnected;
  if (clients != s_lastClientCount || sta != s_lastStaConnected) {
    s_lastClientCount  = clients;
    s_lastStaConnected = sta;
    s_needRedraw = true;
    hal::display::markAllDirty();
  }
}

void onInput(hal::input::InputEvent ev) {
  using ET = hal::input::EventType;
  if (ui::nav::isBackPress(ev)) {
    NavHandler::instance().pop();
  }
}

// ─── Drawing ─────────────────────────────────────────────────────────────────

static void buildQrPayload(const char* ssid, const char* pass, char* out, size_t outLen) {
  if (!out || outLen == 0) return;
  snprintf(out, outLen, "WIFI:T:WPA;S:%s;P:%s;;",
           ssid ? ssid : "Origami", pass ? pass : "");
}

static void setVisible(ui::scene::Node* node, bool visible) {
  if (node) node->visible = visible;
}

static void addQrNode(ui::scene::Scene& scene, const char* ssid, const char* pass) {
  char payload[160];
  buildQrPayload(ssid, pass, payload, sizeof(payload));

  using namespace wifi_portal_layout;
  setVisible(scene.addQrCode("wifi.qr", {kWifiQrX, kWifiQrY, kWifiQrW, kWifiQrH}, payload,
                             kWifiQrModuleSize, kWifiQrFgColor, kWifiQrBgColor, kWifiQrZ),
             kWifiQrVisible);
}

static void buildPortalScene(ui::scene::Scene& scene, int16_t, int16_t) {
  const char* ssid = services::wifi_portal::networkName();
  scene.clear();

  using namespace wifi_portal_layout;

  setVisible(scene.addRect("content.bg", {kContentBgX, kContentBgY, kContentBgW, kContentBgH},
                           kContentBgFillColor, kContentBgRadius, kContentBgZ),
             kContentBgVisible);
  addQrNode(scene, ssid, services::wifi_portal::password());

  setVisible(scene.addLabel("wifi.title", {kWifiTitleX, kWifiTitleY, kWifiTitleW, kWifiTitleH},
                            kWifiTitleText, kWifiTitleFgColor, kWifiTitleBgColor,
                            kWifiTitleTextSize, ui::scene::TextAlign::TopLeft, kWifiTitleZ),
             kWifiTitleVisible);
  setVisible(scene.addLabel("wifi.scan", {kWifiScanX, kWifiScanY, kWifiScanW, kWifiScanH},
                            kWifiScanText, kWifiScanFgColor, kWifiScanBgColor,
                            kWifiScanTextSize, ui::scene::TextAlign::TopLeft, kWifiScanZ),
             kWifiScanVisible);
  setVisible(scene.addLabel("wifi.manual", {kWifiManualX, kWifiManualY, kWifiManualW, kWifiManualH},
                            kWifiManualText, kWifiManualFgColor, kWifiManualBgColor,
                            kWifiManualTextSize, ui::scene::TextAlign::TopLeft, kWifiManualZ),
             kWifiManualVisible);
  setVisible(scene.addLabel("wifi.ssid", {kWifiSsidX, kWifiSsidY, kWifiSsidW, kWifiSsidH},
                            ssid && ssid[0] ? ssid : "Origami_XX",
                            kWifiSsidFgColor, kWifiSsidBgColor,
                            kWifiSsidTextSize, ui::scene::TextAlign::TopLeft, kWifiSsidZ),
             kWifiSsidVisible);
  setVisible(scene.addLabel("wifi.password", {kWifiPasswordX, kWifiPasswordY, kWifiPasswordW, kWifiPasswordH},
                            services::wifi_portal::password(), kWifiPasswordFgColor, kWifiPasswordBgColor,
                            kWifiPasswordTextSize, ui::scene::TextAlign::TopLeft, kWifiPasswordZ),
             kWifiPasswordVisible);
  setVisible(scene.addLabel("wifi.ip", {kWifiIpX, kWifiIpY, kWifiIpW, kWifiIpH},
                            kWifiIpText, kWifiIpFgColor, kWifiIpBgColor,
                            kWifiIpTextSize, ui::scene::TextAlign::TopLeft, kWifiIpZ),
             kWifiIpVisible);

  if (s_lastStaConnected) {
    setVisible(scene.addLabel("wifi.sta", {kWifiStaX, kWifiStaY, kWifiStaW, kWifiStaH},
                              "STA: OK", kWifiStaOkColor, kWifiStaBgColor,
                              kWifiStaTextSize, ui::scene::TextAlign::TopLeft, kWifiStaZ),
               kWifiStaVisible);
    const auto status = services::wifi_portal::status();
    setVisible(scene.addLabel("wifi.sta.ip", {kWifiStaIpX, kWifiStaIpY, kWifiStaIpW, kWifiStaIpH},
                              status.stationAddress, kWifiStaIpFgColor, kWifiStaIpBgColor,
                              kWifiStaIpTextSize, ui::scene::TextAlign::TopLeft, kWifiStaIpZ),
               kWifiStaIpVisible);
  } else {
    setVisible(scene.addLabel("wifi.sta", {kWifiStaX, kWifiStaY, kWifiStaW, kWifiStaH},
                              "AP only", kWifiStaFgColor, kWifiStaBgColor,
                              kWifiStaTextSize, ui::scene::TextAlign::TopLeft, kWifiStaZ),
               kWifiStaVisible);
  }

  char clientText[24];
  snprintf(clientText, sizeof(clientText), "Clients: %d", s_lastClientCount < 0 ? 0 : s_lastClientCount);
  setVisible(scene.addLabel("wifi.clients", {kWifiClientsX, kWifiClientsY, kWifiClientsW, kWifiClientsH},
                            clientText, kWifiClientsFgColor, kWifiClientsBgColor,
                            kWifiClientsTextSize, ui::scene::TextAlign::TopLeft, kWifiClientsZ),
             kWifiClientsVisible);

}

void onDraw(TFT_eSprite& canvas) {
  if (!s_needRedraw) return;
  s_needRedraw = false;

  ui::scene::Scene& scene = ui::scene::scratchScene();
  buildPortalScene(scene, canvas.width(), canvas.height());
  scene.render(canvas);
}

static void screenTick(uint32_t) {
  tick();
}

static const ::scene::Screen kScreenWifiPortal = {
    "WiFi Portal",
    enter,
    nullptr,
    screenTick,
    onInput,
    nullptr,
    onDraw,
    "portal",
    buildPortalScene,
    &screen_chrome_layout::kWifiPortalTopBarColor,
};

static const ::scene::Screen* const kScreens[] = {&kScreenWifiPortal};
App app = SCENE_APP_CATALOG_LIFECYCLE("WiFi Portal", "wifi-portal", &kScreenWifiPortal, beginSession, exitApp,
                            "src/apps/wifi_portal_app.cpp", kScreens);

}  // namespace wifi_portal
}  // namespace apps
