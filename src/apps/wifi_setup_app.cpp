/**
 * @file wifi_setup_app.cpp
 * @brief WiFi provisioning via camera QR scan (direct-panel app).
 */

#include "core/telemetry.h"
#include "apps/wifi_setup_app.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_camera.h>
#include <string.h>
#include <math.h>

#include "app.h"
#include "apps/agent/agent_flow.h"
#include "services/uploads.h"
#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "core/menu_config.h"
#include "services/qr_decode.h"
#include "core/wifi_qr_parser.h"
#include "core/secrets.h"
#include "hal/hal_ble.h"
#include "hal/hal_camera.h"
#include "services/wifi_qr_session.h"
#include "services/net_poll.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "hal/hal_sd.h"
#include "hal/hal_wifi.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "ui/draw.h"
#include "ui/nav_controls.h"
#include "ui/scene.h"
#include "ui/source_layout.h"
#include "ui/onboarding_scanner_view.h"
#include "ui/qr_scanner_view.h"
#include "ui/qr_preview.h"

#include <stdio.h>

namespace apps {
namespace wifi_setup {

enum class State {
  PREPARING,
  SCANNING,
  RELEASING,
  CONNECTING,
  SUCCESS,
  ERROR,
};

static ui::QrPreview s_preview;
static bool      s_chromeDrawn  = false;

static bool     s_displayBypass = false;
static bool s_tutorialMode=false;
static onboarding::WifiResult s_tutorialResult=onboarding::WifiResult::None;
static uint32_t s_scanStartMs=0,s_lastFrameMs=0,s_resultMs=0;
static State    s_state         = State::PREPARING;
static uint32_t s_connectStartMs = 0;
static uint32_t s_scanAttempts  = 0;
static uint32_t s_frames = 0;
static uint32_t s_prepareStartMs = 0;
static uint32_t s_lastProgressMs = 0;
static auto& s_session = services::wifi_qr::Session::instance();

static bool s_needsScanRequest = true;
static uint32_t s_successMs = 0;
static uint32_t s_activityStartMs = 0;
static uint32_t s_activityFrameMs = 0;
static constexpr const char* kActivityDots[] = {
  "wifi_setup.dot0", "wifi_setup.dot1", "wifi_setup.dot2"
};
static core::wifi_qr::Credentials s_credentials{};
static char     s_statusLine[64] = {0};
static char     s_detailLine[64] = {0};
static char     s_lastPayload[256] = {0};

static void copyField(const char* src, char* dst, size_t cap) {
  if (!dst || cap == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

static inline TFT_eSPI& panel() {
  return s_displayBypass ? hal::display::directTft() : hal::display::tft();
}

static void setDisplayBypass(bool on) {
  if (on == s_displayBypass) return;
  s_displayBypass = on;
  if (on) hal::display::enterOptimizedRender();
  else    hal::display::exitOptimizedRender();
}

static void buildScanningScene(ui::scene::Scene& scene, int16_t width, int16_t height,
                               bool /*scanning*/) {
  scene.clear();
  ui::scene::setCurrentScene(&scene);
  auto* preview = scene.addRect("wifi_setup.preview", {0, 0, width, height}, TFT_BLACK);
  if (preview) preview->visible = false;
  // Scene metadata remains available to the device inspector. The shared view
  // below draws the native-pixel overlay onto the actual camera framebuffer.
  auto* guide = scene.addRect("wifi_setup.guide", {60, 40, 161, 161}, TFT_WHITE);
  if (guide) guide->visible = false;
  scene.addRect("wifi_setup.scan_band", {56, 207, 200, 19}, TFT_BLACK);
  scene.addLabel("wifi_setup.scan_text", {56, 213, 200, 8},
                 "Place QR code inside frame", TFT_WHITE, TFT_BLACK,
                 1, ui::scene::TextAlign::TopCenter);

}

static void buildScannerPreviewScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  buildScanningScene(scene, width, height, false);
}

static void publishScanningScene(bool scanning) {
  TFT_eSPI& d = panel();
  ui::scene::Scene& scene = ui::scene::scratchScene();
  buildScanningScene(scene, d.width(), d.height(), scanning);
}

// There is intentionally no app-local resumeBleIfNeeded(). The Core-0 WiFi
// manager owns the transition after camera exit: it either restores STA while
// BLE remains blocked, or keeps WiFi fully off before entering OfflineBle.

struct ScannerCanvas {
  TFT_eSprite& frame;
  uint16_t at(int x, int y) const { return frame.readPixel(x, y); }
  void fillRect(int x, int y, int w, int h, uint16_t ink) {
    frame.fillRect(x, y, w, h, ink);
  }
};

static void drawViewfinderOverlay(bool scanning) {
  publishScanningScene(scanning);
  auto& frame = hal::display::canvas();
  ScannerCanvas canvas{frame};
  qr_scanner_view::overlay(canvas);
  hal::display::beginDirectDraw();
  qr_scanner_view::regions([&](int x, int y, int w, int h) {
    frame.pushSprite(x, y, x, y, w, h);
  });
  hal::display::endDirectDraw();
}

static void drawStaticChrome() {
  auto& frame = hal::display::canvas();
  frame.resetViewport();
  frame.fillSprite(TFT_BLACK);
  hal::display::beginDirectDraw();
  panel().fillScreen(TFT_BLACK);
  hal::display::endDirectDraw();
  drawViewfinderOverlay(false);
  s_chromeDrawn = true;
}

static bool renderAndPrepareScan(const hal::camera::CameraFrame& frame) {
  return s_preview.render(frame.buf,frame.len);
}

static uint16_t backgroundColor() {
  if (s_state == State::SUCCESS) return kUiSuccessBg;
  if (s_state == State::ERROR) return kUiErrorBg;
  return kUiBg;
}

static void animateActivity(uint32_t now) {
  if (now - s_activityFrameMs < 33) return;
  auto& scene = ui::scene::scratchScene();
  if (!scene.find(kActivityDots[0])) return;
  s_activityFrameMs = now;
  hal::display::beginDirectDraw();
  TFT_eSPI& d = panel();
  // Erase only the previous dots; keep the title, details and chrome still.
  d.fillRect(d.width() / 2 - 22, 77, 45, 18, kUiBg);
  for (unsigned i = 0; i < 3; ++i) {
    auto* dot = scene.find(kActivityDots[i]);
    if (!dot) continue;
    const uint32_t phase = ((now - s_activityStartMs) % 1000 + 1000 - i * 140) % 1000;
    const float wave = phase < 600 ? sinf(3.14159265f * phase / 600.0f) : 0.0f;
    dot->rect.y = 90 - (int16_t)(9.0f * wave * wave + 0.5f);
    d.fillCircle(dot->rect.x, dot->rect.y, 3, kUiMuted);
  }
  hal::display::endDirectDraw();
  hal::display::markDirty(77, 18);
  if(s_tutorialMode){
    hal::display::beginDirectDraw();d.fillRect(0,188,280,52,kUiBg);
    onboarding_wifi::footer(d,now-s_activityStartMs,"Press A to go back");
    hal::display::endDirectDraw();hal::display::markDirty(188,52);
  }
}

static void drawStatusScreen(const char* title, const char* line1, const char* line2,
                             bool ok) {
  if(s_tutorialMode)line2=nullptr;
  hal::display::beginDirectDraw();
  TFT_eSPI& d = panel();
  ui::scene::Scene& scene = ui::scene::scratchScene();
  scene.clear();
  ui::scene::setCurrentScene(&scene);
  ui::scene::Builder b(scene, d.width(), d.height());
  const uint16_t bg = backgroundColor();
  b.clearContent(bg);
  const int16_t w = d.width();
  const int16_t cx = w / 2;
  const bool result = s_state == State::SUCCESS || s_state == State::ERROR;
  if (result) {
    scene.addCircle("wifi_setup.result", cx, 90, 23, kUiFg, 2);
    // Geometric marks work with the firmware's existing font and theme.
    if (ok) {
      for (int i = -1; i <= 1; ++i) {
        char id[32];
        snprintf(id, sizeof(id), "wifi_setup.check%d.a", i);
        scene.addLine(id, cx-12, 90+i, cx-3, 99+i, kUiFg, 1);
        snprintf(id, sizeof(id), "wifi_setup.check%d.b", i);
        scene.addLine(id, cx-3, 99+i, cx+13, 81+i, kUiFg, 1);
      }
    } else {
      scene.addLine("wifi_setup.cross.a", cx-9, 81, cx+9, 99, kUiFg, 1);
      scene.addLine("wifi_setup.cross.b", cx-9, 99, cx+9, 81, kUiFg, 1);
    }
  } else {
    s_activityStartMs = s_activityFrameMs = millis();
    for (int i = 0; i < 3; ++i) {
      scene.addCircle(kActivityDots[i], cx + (i - 1) * 14, 90, 3,
                      kUiMuted, 0, kUiMuted, true);
    }
  }
  scene.addLabel("wifi_setup.status", {14, 126, (int16_t)(w-28), 20},
                 title, kUiFg, bg, strlen(title)>20?1:2, ui::scene::TextAlign::TopCenter);
  if (line1 && line1[0]) scene.addLabel("wifi_setup.detail", {14, 158, (int16_t)(w-28), 16},
                 line1, kUiMuted, bg, 1, ui::scene::TextAlign::TopCenter);
  if (line2 && line2[0]) scene.addLabel("wifi_setup.action", {14,190,(int16_t)(w-28),24},
      line2,kUiFg,bg,2,ui::scene::TextAlign::MiddleCenter);
  if(line1 && strlen(line1)>40){
    auto* detail=scene.find("wifi_setup.detail");
    std::string all=line1;size_t cut=all.rfind(' ',40);if(cut==std::string::npos)cut=40;
    if(detail)snprintf(detail->text,sizeof(detail->text),"%s",all.substr(0,cut).c_str());
    scene.addLabel("wifi_setup.detail2",{14,174,252,12},all.substr(cut+1).c_str(),kUiMuted,bg,1,ui::scene::TextAlign::TopCenter);
  }
  if(s_tutorialMode && s_state==State::ERROR)
    scene.addLabel("wifi_setup.retry",{24,32,72,10},"Try again",kUiFg,bg,1);
  scene.render(d);
  if(s_tutorialMode){
    // These pages share the preview's arrows and retain the actual scanner result.
    d.fillRect(0,0,280,kSystemStatusBarH,bg);
    onboarding_wifi::arrows(d,!result,result);
    if(s_state==State::ERROR){
      // The scene label supplies inspection metadata; use the shared pixel font
      // for the final control, clearing the scene font's different baseline.
      d.fillRect(20,30,90,18,bg);
      onboarding_wifi::retryControl(d);
    }
    if(!result)onboarding_wifi::footer(d,0,"Press A to go back");
  }
  hal::display::endDirectDraw();
  if (!s_displayBypass) hal::display::markAllDirty();
}

static void logStartupStage(const char* stage) {
  Serial.printf("[wifi-app] startup id=%lu stage=%s free=%u largest=%u tasks=%u ioPaused=%u agentPaused=%u agentActive=%u agentReady=%u\n",
                (unsigned long)s_session.id(), stage,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)uxTaskGetNumberOfTasks(), s_session.backgroundHeld(), s_session.transportHeld(),
                apps::agent::flow::isSessionActive(), apps::agent::flow::isTransportReady());
}

static void beginConnect(const core::wifi_qr::Credentials& creds) {
  s_connectStartMs = millis();
  s_credentials = creds;
  s_preview.close();
  s_session.stopCapture();
  setDisplayBypass(false);
  s_state = State::RELEASING;
  copyField(creds.ssid, s_statusLine, sizeof(s_statusLine));
  drawStatusScreen(s_tutorialMode?"Connecting...":"Joining Wi-Fi...", s_statusLine, "Left: cancel", false);
}

static void showError(const char* reason, const char* title = "Couldn't join") {
  LOGW("camera-qr", "session failed: %s", reason);
  Serial.printf("[wifi-app] error id=%lu title=%s reason=%s\n",
                (unsigned long)s_session.id(), title, reason);
  s_preview.close();
  s_session.fail();
  setDisplayBypass(false);
  s_state = State::ERROR;
  s_resultMs=millis();
  memset(&s_credentials, 0, sizeof(s_credentials));
  s_lastPayload[0] = '\0';
  drawStatusScreen(title, reason, "Press: scan again  Left: back", false);
}

#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool debugJoin(bool savedNetwork, bool accountPayload) {
  if (NavHandler::instance().top() != &app || s_state != State::SCANNING) return false;
  core::wifi_qr::Credentials credentials{};
  if (savedNetwork) {
    if (core::secrets::wifiNetworkCount() == 0) return false;
    copyField(core::secrets::wifiNetworkSSID(0), credentials.ssid, sizeof(credentials.ssid));
    copyField(core::secrets::wifiNetworkPassword(0), credentials.pass, sizeof(credentials.pass));
  } else {
    copyField("qr-test-unavailable-53820c", credentials.ssid, sizeof(credentials.ssid));
    copyField("test-only-password", credentials.pass, sizeof(credentials.pass));
  }
  if(accountPayload){
    const auto backend=core::secrets::backendConfig();
    if(!backend.apiKey[0])return false;
    JsonDocument doc;doc["s"]=credentials.ssid;doc["p"]=credentials.pass;doc["a"]=backend.apiKey;
    char payload[512];const size_t n=serializeJson(doc,payload,sizeof(payload));
    core::wifi_qr::Credentials parsed{};
    if(n>=sizeof(payload)||!core::wifi_qr::parsePayload(payload,&parsed)||!parsed.hasApiKey)return false;
    credentials=parsed;memset(payload,0,sizeof(payload));
    Serial.println("[wifi-app] diagnostic compact account payload parsed apiKeyPresent=1");
  }
  beginConnect(credentials);
  return true;
}
#endif

static void handlePayload(const char* payload) {
  if (s_state != State::SCANNING) return;
  if (!payload || !payload[0]) return;
  if (strcmp(payload, s_lastPayload) == 0) return;
  copyField(payload, s_lastPayload, sizeof(s_lastPayload));

  core::wifi_qr::Credentials creds;
  if (!core::wifi_qr::parsePayload(payload, &creds)) {
    Serial.println("[wifi-app] invalid or unsupported Wi-Fi QR");
    showError("Use a supported Wi-Fi QR code", "Can't use this QR");
    return;
  }

  Serial.printf("[wifi-app] QR parsed id=%lu credentials=valid apiKeyPresent=%u\n",
                (unsigned long)s_session.id(), creds.hasApiKey);
  beginConnect(creds);
}

#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool debugPayload(const char* payload){
 if(NavHandler::instance().top()!=&app||s_state!=State::SCANNING||!payload||strlen(payload)>360)return false;
 handlePayload(payload);return true;
}
#endif

static void prepareWorkflow() {
  Serial.println("[wifi-app] Entering WiFi QR setup");
  s_state = State::PREPARING;
  s_connectStartMs = 0;
  s_scanAttempts = 0;
  s_frames = 0;
  s_prepareStartMs = s_lastProgressMs = millis();
  memset(&s_credentials, 0, sizeof(s_credentials));
  s_statusLine[0] = s_detailLine[0] = s_lastPayload[0] = '\0';
  s_needsScanRequest = true;
}

static bool s_registrationQueued = false;

static void enterView() {
  // Paint first; tick pauses network users BEFORE asking the manager to stop
  // the radio. Retry keeps an already-held pause, without nesting ownership.
  drawStatusScreen("Starting scanner...", nullptr, "Left: back", false);
}

static void retireView() {
  s_preview.close();
  setDisplayBypass(false);
  s_chromeDrawn = false;
  memset(&s_credentials, 0, sizeof(s_credentials));
  memset(s_lastPayload, 0, sizeof(s_lastPayload));
}

void exitApp() {
  s_tutorialMode=false;
  Serial.println("[wifi-app] Exiting WiFi QR setup");
  (void)s_session.close(retireView);
}

void tick() {
  const uint32_t now = millis();
  const auto status = hal::wifi::qrStatus();

  if (s_state == State::PREPARING || s_state == State::RELEASING ||
      s_state == State::CONNECTING) animateActivity(now);

  if (s_state == State::PREPARING) {
    if (s_needsScanRequest) {
      if (!s_session.begin(logStartupStage)) {
        showError("Network busy. Please try again", "QR scanner failed");
        return;
      }
      s_needsScanRequest = false;
      s_prepareStartMs = millis();
      return;
    }
    if (now - s_prepareStartMs >= 10000) {
      showError("Startup timed out. Try again", "QR scanner failed");
      return;
    }
    const auto decoder = s_session.prepareDecoder(logStartupStage);
    if (decoder == services::wifi_qr::Session::Decoder::Waiting) return;
    if (decoder == services::wifi_qr::Session::Decoder::RadioFailed) {
      showError("Wi-Fi could not stop", "QR scanner failed");
      return;
    }
    if (decoder == services::wifi_qr::Session::Decoder::Failed) {
      showError(s_session.decoderError(), "QR scanner failed");
      return;
    }
    setDisplayBypass(true);
    if (!s_session.openCamera()) { showError(s_session.cameraError(), "Camera unavailable"); return; }
    hal::display::fixColorInversion();
    hal::input::configureRotaryEncoderPins();
    logStartupStage("camera-ready");
    s_state = State::SCANNING;
    s_scanStartMs=s_lastFrameMs=millis();
    drawStaticChrome();
    return;
  }

  if (s_state == State::RELEASING) {
    const auto join = s_session.join(s_credentials, s_connectStartMs);
    if (join == services::wifi_qr::Session::Join::Waiting) {
      if (now - s_connectStartMs >= 3000) showError("Scanner couldn't stop");
      return;
    }
    if (join == services::wifi_qr::Session::Join::Rejected) {
      showError("Please scan again");
    } else {
      s_registrationQueued = false;
      s_state = State::CONNECTING;
      memset(&s_credentials, 0, sizeof(s_credentials));
      memset(s_lastPayload, 0, sizeof(s_lastPayload));
    }
    return;
  }

  if (s_state == State::ERROR) {
    // Release both camera and decoder before restoring network users so these
    // queued diagnostics can reach Fleet even while the error page is open.
    (void)s_session.releaseStopped();
    return;
  }

  char payload[256];
  if (services::qr::pollPayload(payload, sizeof(payload))) {
    handlePayload(payload);
  }

  if(s_state==State::SCANNING && now-s_scanStartMs>=60000){
    showError("No Wi-Fi code read within 60 seconds", "QR scan timed out");return;
  }
  if(s_state==State::SCANNING && now-s_lastFrameMs>=8000){
    showError("Camera supplied no frames for 8 seconds", "No camera frames");return;
  }
  if (s_state == State::SCANNING) {
    const auto frame = s_session.renderFrame(renderAndPrepareScan);
    using F = services::wifi_qr::Session::Frame;
    if (frame == F::DecoderStopped) {
      showError("Decoder stopped. Please retry", "QR scanner failed");
      return;
    }
    if (frame == F::Rendered || frame == F::Submitted) {
      ++s_frames; s_lastFrameMs = now;
      if (frame == F::Submitted) ++s_scanAttempts;
    }
    if (now - s_lastProgressMs >= 5000) {
      s_lastProgressMs = now;
      Serial.printf("[wifi-app] scan id=%lu frames=%lu submitted=%lu elapsed=%lu\n",
                    (unsigned long)s_session.id(), (unsigned long)s_frames,
                    (unsigned long)s_scanAttempts, (unsigned long)(now - s_prepareStartMs));
      services::qr::logDiagnostics();
    }
    return;
  }

  if (s_state == State::CONNECTING) {
    if (status.id != s_session.id()) return;
    if (status.phase == hal::wifi::QrPhase::Joined) {
      // Joining has committed the credentials and the scanner is stopped.
      // Release its networking reservation before showing Connected, rather
      // than making website registration wait for the user to press B.
      if (s_tutorialMode && !s_registrationQueued) {
        services::net_poll::requestOnboardingRegistration();
        s_registrationQueued = true;
      }
      if (!s_session.releaseStopped()) return;
      s_state = State::SUCCESS;
      s_resultMs=millis();
      drawStatusScreen("Connected", s_statusLine, nullptr, true);
      s_successMs = millis();
      return;
    }
    if (status.phase == hal::wifi::QrPhase::Failed) {
      using E = hal::wifi::QrError;
      switch (status.error) {
        case E::NetworkNotFound: showError("Network not found"); break;
        case E::Authentication: showError("Check network password"); break;
        case E::NetworksFull: showError("Remove one in Settings", "Saved networks full"); break;
        case E::NoIp: showError("Couldn't get an IP address"); break;
        case E::SaveFailed: showError("Settings couldn't be saved", "Joined, not saved"); break;
        case E::Radio: showError("Wi-Fi couldn't start"); break;
        default: showError("Connection timed out"); break;
      }
    }
  } else if (s_state == State::SUCCESS && !s_tutorialMode && now - s_successMs >= 1200) {
    NavHandler::instance().pop();
  }
}

void onInput(hal::input::InputEvent ev) {
  using ET = hal::input::EventType;
  if(s_tutorialMode){
    const bool result=s_state==State::SUCCESS||s_state==State::ERROR;
    if(result && millis()-s_resultMs<600)return;
    if(ui::nav::isBackTap(ev) && s_state==State::ERROR){
      // Reuse startup ownership/cleanup gates; remain in this tutorial scanner.
      s_tutorialResult=onboarding::WifiResult::None;prepareWorkflow();enterView();return;
    }
    if(ev.type==ET::ButtonB_Tap && s_state!=State::CONNECTING && s_state!=State::RELEASING){
      s_tutorialResult=s_state==State::SUCCESS?onboarding::WifiResult::Joined:s_state==State::ERROR?onboarding::WifiResult::Failed:onboarding::WifiResult::Skipped;
      NavHandler::instance().pop();
    }else if(ui::nav::isBackTap(ev) && !result){
      s_tutorialResult=onboarding::WifiResult::Back;NavHandler::instance().pop();
    }
    return;
  }
  if (ui::nav::isBackTap(ev) ||
      (ev.type == ET::ButtonB_Tap && (s_state == State::SCANNING || s_state == State::PREPARING))) {
    NavHandler::instance().pop();
    return;
  }
  if (ui::nav::isEnterTap(ev)) {
    if (s_state == State::ERROR) { prepareWorkflow(); enterView(); }
    else if (s_state == State::SUCCESS) NavHandler::instance().pop();
    return;
  }
}

bool tutorialActive(){return s_tutorialMode;}
void beginTutorial(){
 s_tutorialMode=true;s_tutorialResult=onboarding::WifiResult::None;
 NavHandler::instance().push(&app);
}
onboarding::WifiResult takeTutorialResult(){auto result=s_tutorialResult;s_tutorialResult=onboarding::WifiResult::None;return result;}
static bool onChord(){
 if(!s_tutorialMode)return false;
 hal::input::InputEvent ev;ev.type=hal::input::EventType::ButtonA_Tap;onInput(ev);return true;
}
static void screenTick(uint32_t) {
  tick();
}

static const ::scene::Screen kScreenWifiSetup = {
    "WiFi Setup",
    enterView,
    nullptr,
    screenTick,
    onInput,
    onChord,
    nullptr,
    "qr-scan",
    buildScannerPreviewScene,
    nullptr, backgroundColor,
};

static const ::scene::Screen* const kScreens[] = {&kScreenWifiSetup};
static AppPresentation presentationPolicy() {
    const bool tutorial=tutorialActive();
    return {false, tutorial, tutorial ? AppPresentation::StatusBar::Hidden : AppPresentation::StatusBar::Default};
}
App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("WiFi Setup", "wifi-setup", &kScreenWifiSetup, prepareWorkflow, prepareWorkflow, exitApp,
                            "src/apps/wifi_setup_app.cpp", kScreens, presentationPolicy, nullptr);

}  // namespace wifi_setup
}  // namespace apps
