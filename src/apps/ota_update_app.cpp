#include "config/screen_chrome_layout.h"
#include "apps/ota_update_app.h"

#include <Arduino.h>
#include <cstring>
#include <atomic>

#include "app.h"
#include "services/firmware_update.h"
#include "core/menu_config.h"
#include "services/ota_client.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "hal/hal_wifi.h"
#include "nav_handler.h"
#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "config/ota_update_layout.h"
#include "scene/screen.h"
#include "ui/nav_controls.h"
#include "ui/scene.h"
#include "ui/morning_style.h"

namespace apps {
namespace ota_update {

enum State {
    ST_CONFIRM,
    ST_CHECKING,     // fetching signed channel manifest
    ST_FOUND,        // newer version available — waiting for user confirm
    ST_UP_TO_DATE,   // already on latest
    ST_NO_WIFI,      // no WiFi connection
    ST_DOWNLOADING,  // streaming binary to OTA partition
    ST_FLASHING,     // finalising OTA write
    ST_SUCCESS,      // OTA committed, rebooting
    ST_ERROR,        // something went wrong
};

static State    s_state        = ST_CHECKING;
static bool     s_needRedraw   = false;
static bool     s_flashBegun   = false;
static int      s_written      = 0;
static int      s_total        = 0;
static uint32_t s_progressRedrawMs = 0;
static uint32_t s_successMs    = 0;
static int      s_checkRetries = 0;   // attempts made so far in CHECKING
static uint32_t s_retryAfterMs = 0;   // millis() timestamp; retry when elapsed
static bool     s_appActive    = false;
static services::firmware_update::Session s_session;
static const char* s_resourceError = nullptr;

static bool reserveNetwork() {
    using Result = services::firmware_update::Session::Result;
    switch (s_session.reserveNetwork()) {
    case Result::Ready: return true;
    case Result::BackgroundBusy: s_resourceError = "background network busy"; break;
    case Result::AgentBusy: s_resourceError = "voice network busy"; break;
    }
    return false;
}
static services::ota_client::ReleaseChannel s_channel =
    services::ota_client::ReleaseChannel::Production;

static services::ota_client::ReleaseInfo s_release;

extern const ::scene::Screen kScreenOtaCheck;
extern const ::scene::Screen kScreenOtaProgress;

bool isActive() {
    return s_appActive && (s_state == ST_CHECKING || s_state == ST_DOWNLOADING || s_state == ST_FLASHING);
}

static void setState(State st) {
    s_state      = st;
    s_needRedraw = true;
    hal::display::markAllDirty();
    if (!s_appActive) return;
    const bool progress =
        st == ST_DOWNLOADING || st == ST_FLASHING || st == ST_SUCCESS;
    const ::scene::Screen* target = progress ? &kScreenOtaProgress : &kScreenOtaCheck;
    if (app.currentScreen != target) ::scene::switchTo(&app, target, progress ? ::scene::ScreenTransition::Forward : ::scene::ScreenTransition::Back);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void enter() {
    s_appActive    = true;
    s_resourceError = nullptr;
    s_flashBegun   = false;
    s_written      = 0;
    s_total        = 0;
    s_checkRetries = 0;
    s_retryAfterMs = 0;
    s_release      = {};
    s_channel      = services::ota_client::releaseChannel();
    if (services::firmware_update::noticeBusy()) {
        setState(ST_CHECKING);
        return;
    }
    services::ota_client::abortFlash();
    if (hal::wifi::connected() && services::firmware_update::settingsUpdateAvailable()) {
        services::firmware_update::copySettingsRelease(&s_release);
        setState(ST_FOUND);
    } else if (!hal::wifi::connected()) {
        setState(ST_NO_WIFI);
    } else {
        setState(ST_CONFIRM);
    }
}

void exitApp() {
    services::ota_client::abortFlash();
    if (!services::firmware_update::noticeBusy()) s_session.close();
    s_flashBegun = false;
    s_appActive = false;
}

// ---------------------------------------------------------------------------
// Tick — state machine pump
// ---------------------------------------------------------------------------

void tick() {
    switch (s_state) {

        case ST_CHECKING: {
            if (services::firmware_update::noticeBusy()) return;
            if (!reserveNetwork()) { setState(ST_ERROR); return; }
            if (!hal::wifi::connected()) { setState(ST_NO_WIFI); return; }
            // Wait out the retry delay before issuing the next attempt.
            if (s_retryAfterMs != 0) {
                if ((uint32_t)(millis() - s_retryAfterMs) < 3000u) return;
                s_retryAfterMs = 0;
            }
            if (!services::ota_client::fetchLatestRelease(s_channel, &s_release)) {
                if (s_checkRetries < 3) {
                    s_checkRetries++;
                    s_retryAfterMs = millis();
                    s_needRedraw   = true;
                    return;
                }
                setState(ST_ERROR);
                return;
            }
            // Only offer a candidate that is actually newer. This avoids
            // repeatedly prompting for an older release after a local build.
            if (services::ota_client::isReleaseNewer(s_channel, s_release.tag)) {
                setState(ST_FOUND);
            } else {
                setState(ST_UP_TO_DATE);
            }
            break;
        }

        case ST_CONFIRM:
        case ST_FOUND:
        case ST_UP_TO_DATE:
        case ST_NO_WIFI:
        case ST_ERROR:
            break;  // idle — waiting for user input

        case ST_DOWNLOADING: {
            if (!s_flashBegun) {
                if (!reserveNetwork()) { setState(ST_ERROR); return; }
                if (!services::ota_client::beginFlash(s_release)) {
                    setState(ST_ERROR);
                    return;
                }
                s_flashBegun = true;
            }
            const bool still_going = services::ota_client::pumpFlash(&s_written, &s_total);
            if (!still_going || (uint32_t)(millis() - s_progressRedrawMs) >= 250u) {
                s_progressRedrawMs = millis();
                s_needRedraw = true;
                hal::display::markAllDirty();
            }
            if (!still_going) {
                if (services::ota_client::lastError()[0] != '\0') {
                    setState(ST_ERROR);
                } else {
                    setState(ST_FLASHING);
                }
            }
            break;
        }

        case ST_FLASHING: {
            if (!services::ota_client::finishFlash()) {
                setState(ST_ERROR);
            } else {
                s_successMs = millis();
                setState(ST_SUCCESS);
            }
            break;
        }

        case ST_SUCCESS: {
            if ((uint32_t)(millis() - s_successMs) >= 2000u) {
                delay(100);
                ESP.restart();
            }
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void onInput(hal::input::InputEvent ev) {
    using ET = hal::input::EventType;
    if (ev.type != ET::ButtonA_Tap && ev.type != ET::ButtonC_Tap) return;

    if (ui::nav::isEnterTap(ev) && s_state == ST_CONFIRM) {
        setState(ST_CHECKING);
        return;
    }
    if (ui::nav::isEnterTap(ev) && s_state == ST_FOUND) {
        s_flashBegun = false;
        setState(ST_DOWNLOADING);
        return;
    }

    if (ui::nav::isEnterTap(ev) && s_state == ST_ERROR) {
        s_resourceError = nullptr;
        s_checkRetries = 0;
        s_retryAfterMs = 0;
        setState(ST_CHECKING);
        return;
    }

    // LB: cancel / back from any idle state
    if (ui::nav::isBackTap(ev)) {
        s_session.cancel();
        NavHandler::instance().pop();
    }
}

bool onChord() {
    s_session.cancel();
    return false;  // let NavHandler pop
}

// ---------------------------------------------------------------------------
// Draw
// ---------------------------------------------------------------------------

static void setVisible(ui::scene::Node* node, bool visible) {
    if (node) node->visible = visible;
}

static void addContent(ui::scene::Scene& scene) {
    using namespace ota_update_layout;
    setVisible(scene.addRect("content.bg", {kContentBgX, kContentBgY, kContentBgW, kContentBgH},
                             kContentBgFillColor, kContentBgRadius, kContentBgZ),
               kContentBgVisible);
}

static void addHeader(ui::scene::Scene& scene, const char* title) {
    using namespace ota_update_layout;
    setVisible(scene.addLabel("update.title",
                               {kHeaderTitleX, kHeaderTitleY, kHeaderTitleW, kHeaderTitleH},
                               title, kHeaderTitleFgColor, kHeaderTitleBgColor,
                               kHeaderTitleTextSize, ui::scene::TextAlign::TopLeft, kHeaderTitleZ),
               kHeaderTitleVisible);
}

static void addStatus(ui::scene::Scene& scene, const char* id, const char* text,
                      uint16_t color, uint8_t slot) {
    using namespace ota_update_layout;
    const int16_t x = slot == 0 ? kStatus0X : (slot == 1 ? kStatus1X : kStatusAfterProgressX);
    const int16_t y = slot == 0 ? kStatus0Y : (slot == 1 ? kStatus1Y : kStatusAfterProgressY);
    const int16_t w = slot == 0 ? kStatus0W : (slot == 1 ? kStatus1W : kStatusAfterProgressW);
    const int16_t h = slot == 0 ? kStatus0H : (slot == 1 ? kStatus1H : kStatusAfterProgressH);
    const uint8_t textSize = slot == 0 ? kStatus0TextSize : (slot == 1 ? kStatus1TextSize : kStatusAfterProgressTextSize);
    const uint8_t z = slot == 0 ? kStatus0Z : (slot == 1 ? kStatus1Z : kStatusAfterProgressZ);
    const bool visible = slot == 0 ? kStatus0Visible : (slot == 1 ? kStatus1Visible : kStatusAfterProgressVisible);
    const uint16_t bg = slot == 0 ? kStatus0BgColor : (slot == 1 ? kStatus1BgColor : kStatusAfterProgressBgColor);
    setVisible(scene.addLabel(id, {x, y, w, h}, text, color, bg, textSize,
                              ui::scene::TextAlign::TopLeft, z),
               visible);
}

static void addDownloadProgress(ui::scene::Scene& scene, uint32_t value, uint32_t max) {
    using namespace ota_update_layout;
    setVisible(scene.addProgressBar("progress.download",
                                    {kProgressDownloadX, kProgressDownloadY,
                                     kProgressDownloadW, kProgressDownloadH},
                                    value, max, kProgressDownloadTrackColor,
                                    kProgressDownloadFillColor,
                                    kProgressDownloadRadius, kProgressDownloadZ),
               kProgressDownloadVisible);
}

static void addFooter(ui::scene::Scene& scene, const char* text) {
    // Back is consistent across idle screens; no instruction strip.
}

static void buildOtaScene(ui::scene::Scene& scene, State state) {
    scene.clear();
    addContent(scene);

    char channelTitle[32];
    snprintf(channelTitle, sizeof(channelTitle), "%s Update",
             services::ota_client::releaseChannelName(s_channel));

    switch (state) {
        case ST_CONFIRM:
            ui::kami_message::message(scene,280,240,"update.confirm",
                "Check for a firmware update?");
            ui::kami_message::footer(scene,280,240,"C: Check","A: Back");
            break;

        case ST_CHECKING:
            addHeader(scene, channelTitle);
            if (s_checkRetries > 0) {
                char retry[40];
                snprintf(retry, sizeof(retry), "Checking... (%d/3)", s_checkRetries);
                addStatus(scene, "status.checking", retry, kUiFg, 0);
            } else {
                addStatus(scene, "status.checking", "Checking updates...", kUiFg, 0);
            }
            break;

        case ST_FOUND: {
            char message[180];
            const char* suffix=strchr(s_release.tag,'-');
            if(suffix)snprintf(message,sizeof(message),"Install update?\n%.*s\n%s",
                int(suffix-s_release.tag),s_release.tag,suffix+1);
            else snprintf(message,sizeof(message),"Install update?\n%s",s_release.tag);
            ui::kami_message::message(scene,280,240,"update.available",message);
            ui::kami_message::footer(scene,280,240,"C: Yes","A: Back");
            break;
        }

        case ST_UP_TO_DATE: {
            addHeader(scene, channelTitle);
            char line[48];
            snprintf(line, sizeof(line), "Up to date");
            addStatus(scene, "status.up_to_date", line, kUiWifiConnected, 0);
            addStatus(scene, "status.version", OTA_FIRMWARE_VERSION, kUiFg, 1);
            addFooter(scene, "LB: Back");
            break;
        }

        case ST_NO_WIFI:
            addHeader(scene, channelTitle);
            addStatus(scene, "status.no_wifi.0", "No WiFi connection.", kUiMuted, 0);
            addStatus(scene, "status.no_wifi.1", "Connect to WiFi first.", kUiMuted, 1);
            addFooter(scene, "LB: Back");
            break;

        case ST_DOWNLOADING:
        case ST_FLASHING:
        case ST_SUCCESS: {
            const bool done=state==ST_SUCCESS;
            const int percent=done ? 100 : (s_total>0 ? int(int64_t(s_written)*100/s_total) : 0);
            scene.addLabel("update.title",{20,63,244,24},done?"Update complete":state==ST_FLASHING?"Installing...":"Downloading...",kUiFg,kUiBg,2);
            scene.addRect("progress.outline",{20,104,238,20},kUiFg);
            scene.addRect("progress.inside",{22,106,234,16},kUiBg);
            if(percent>0) scene.addRect("progress.fill",{23,107,int16_t(232*percent/100),14},kUiFg);
            char pct[16];snprintf(pct,sizeof(pct),"%d%%",percent);
            scene.addLabel("status.download_pct",{100,143,80,24},pct,kUiFg,kUiBg,2);
            scene.addLabel("status.warning",{20,184,244,28},done?"Restarting Kami":"Keep Kami powered",kUiMuted,kUiBg,2);
            break;
        }

        case ST_ERROR: {
            addHeader(scene,"Update failed");
            const char* remaining=s_resourceError?s_resourceError:services::ota_client::lastError();
            // Keep diagnostics legible at the standard size instead of shrinking
            // the entire speech bubble when the transport returns a long reason.
            for(int row=0;*remaining && row<5;++row) {
                const auto line=ui::default_font::line(remaining,244,2);
                if(!line.next)break;
                char text[128],id[32];memcpy(text,remaining,line.bytes);text[line.bytes]=0;
                snprintf(id,sizeof(id),"status.error.%d",row);
                scene.addLabel(id,{18,int16_t(76+row*22),244,22},text,kUiFg,kUiBg,2);
                remaining+=line.next;
            }
            ui::kami_message::footer(scene,280,240,"C: Retry","A: Back");
        }
            break;
    }
}

static void buildCheckScene(ui::scene::Scene& scene, int16_t, int16_t) {
    const State state = (s_state == ST_DOWNLOADING || s_state == ST_FLASHING || s_state == ST_SUCCESS)
                            ? ST_CHECKING
                            : s_state;
    buildOtaScene(scene, state);
}

static void buildProgressScene(ui::scene::Scene& scene, int16_t, int16_t) {
    const State state = (s_state == ST_DOWNLOADING || s_state == ST_FLASHING || s_state == ST_SUCCESS)
                            ? s_state
                            : ST_DOWNLOADING;
    buildOtaScene(scene, state);
}

void onDraw(TFT_eSprite& canvas) {
    if (!s_needRedraw) return;
    s_needRedraw = false;

    ui::scene::Scene& scene = ui::scene::scratchScene();
    buildOtaScene(scene, s_state);
    scene.render(canvas);
    hal::display::markAllDirty();
}

// ---------------------------------------------------------------------------
// App descriptor
// ---------------------------------------------------------------------------

static void screenTick(uint32_t) {
    tick();
}

static void enterView() {
    s_needRedraw = true;
    hal::display::markAllDirty();
}

const ::scene::Screen kScreenOtaCheck = {
    "Check Update",
    enterView,
    nullptr,
    screenTick,
    onInput,
    onChord,
    onDraw,
    "check",
    buildCheckScene,
    &screen_chrome_layout::kOtaCheckTopBarColor,
};

const ::scene::Screen kScreenOtaProgress = {
    "Progress",
    enterView,
    nullptr,
    screenTick,
    onInput,
    onChord,
    onDraw,
    "progress",
    buildProgressScene,
    &screen_chrome_layout::kOtaProgressTopBarColor,
};

static const ::scene::Screen* const kScreens[] = {
    &kScreenOtaCheck,
    &kScreenOtaProgress,
};
static core::health::RuntimePolicy runtimePolicy() {
    const bool busy = isActive();
    auto policy = core::health::whileBusy(busy);
    policy.allowPowerSleep = !busy;
    return policy;
}
App app = SCENE_APP_CATALOG_LIFECYCLE_POLICIES("Update Firmware", "ota", &kScreenOtaCheck, enter, exitApp,
                            "src/apps/ota_update_app.cpp", kScreens, nullptr, runtimePolicy);

}  // namespace ota_update
}  // namespace apps
