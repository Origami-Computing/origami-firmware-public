/**
 * @file lua_app.cpp
 * @brief Host for sandboxed Lua widgets (Phase 2b).
 *
 * Three screens: loading, a picker that enumerates /widgets/<slug>/main.lua on the SD
 * card (plus the built-in counter), and a runner that drives one LuaVm for
 * the chosen script. App workflow transitions own VM startup and teardown;
 * screen entry only repaints. App resume explicitly retains the established
 * reload behavior after system overlays.
 */

#include "widgets/app.h"
#include "builtin_muni5.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>

#include "config/menu_layout.h"
#include "config/ui_theme.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "hal/hal_sd.h"
#include "widgets/vm.h"
#include "widgets/sync.h"
#include "widgets/launch.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "services.h"
#include "ui/list_view.h"
#include "ui/nav_controls.h"
#include "ui/scene.h"
#include "ui/frame_damage.h"

namespace widgets {

// Defined at file scope below; forward-declared so the screen callbacks can
// reference &app for scene::switchTo before the SCENE_APP_CATALOG definition.
extern App app;

namespace {

// Hard PSRAM ceiling for one widget's heap. Generous for v1 — the eyes/agent
// buffers are not resident while a widget is foreground.
constexpr size_t kMemCeilingBytes = 128u * 1024u;

// Built-in fallback widget: a rotary-adjusted counter. Always listed last in
// the picker so the app works with no SD card at all.
const char kSampleWidget[] = R"LUA(
local count = 0
local bg     = kami.draw.rgb(12, 14, 20)
local accent = kami.draw.rgb(120, 200, 255)
local muted  = kami.draw.rgb(120, 128, 140)

function widget.start()
  count = 0
  kami.log("counter widget started")
end

function widget.draw(now)
  kami.draw.rect("bg", 0, 0, kami.W, kami.H, bg)
  kami.draw.label("title", 0, 18, kami.W, 24, "Lua Counter", 0xFFFF, bg, 2, 1)
  kami.draw.label("val", 0, 86, kami.W, 56, tostring(count), accent, bg, 4, 2)
  kami.draw.progress("bar", 30, 160, kami.W - 60, 14, count % 50, 50,
                kami.draw.rgb(40, 44, 56), accent)
  kami.draw.label("hint", 0, kami.H - 28, kami.W, 20,
             "rotate +/-    press: reset    LB: back", muted, bg, 1, 1)
end

function widget.input(ev)
  if ev.type == "scroll" then
    count = count + ev.delta
  elseif ev.type == "A" then
    count = 0
  end
end
)LUA";

constexpr char kWidgetsDir[]     = "/widgets";
constexpr size_t kMaxScriptBytes = 32u * 1024u;
constexpr size_t kMaxDataBytes   = 8u * 1024u;
constexpr int kMaxWidgets        = 12;  // SD entries; built-in counter is +1
constexpr int kSlugLen           = 32;

// The system status bar (battery/wifi/clock) is always visible: widgets and the
// picker render in the content area BELOW it, and the host chrome is repainted
// on top each frame. Mirrors the native menu's content offset.
constexpr int16_t kBarH = menu_layout::kMenuContentBgY;  // follows the system status strip

// ---------------------------------------------------------------------------
// Picker state
// ---------------------------------------------------------------------------

char s_slugs[kMaxWidgets * kSlugLen];
int  s_sdCount = 0;          // widgets found on SD
int  s_sel = 0;
bool s_needRedraw = false;
ui::ListView s_list;

// Selected widget: slug into s_slugs, or empty string for the built-in.
char s_chosenSlug[kSlugLen] = "";

// Result of the last "sync from kami" action, shown under the picker title.
char s_syncMsg[40] = "";

// Rows: [0, s_sdCount) SD widgets, then built-in counter and Muni 5, then sync action.
int pickerCount() { return s_sdCount + 3; }

const char* pickerLabel(int idx) {
    if (idx >= 0 && idx < s_sdCount) return s_slugs + idx * kSlugLen;
    if (idx == s_sdCount) return "counter (built-in)";
    if (idx == s_sdCount + 1) return "Muni 5 Fulton";
    return "sync from kami";
}

void rescanWidgets() {
    s_sdCount = hal::sd::mounted()
                    ? hal::sd::listDirs(kWidgetsDir, s_slugs, kMaxWidgets, kSlugLen)
                    : 0;
    if (s_sel >= pickerCount()) s_sel = 0;
}

void runSync() {
    char err[48];
    const int n = widgets::sync::syncFromBackend(err, sizeof(err));
    if (n < 0) {
        snprintf(s_syncMsg, sizeof(s_syncMsg), "sync failed: %s", err);
    } else {
        snprintf(s_syncMsg, sizeof(s_syncMsg), "synced %d widget%s", n,
                 n == 1 ? "" : "s");
    }
    Serial.printf("[lua-app] %s\n", s_syncMsg);
    rescanWidgets();
}

// ---------------------------------------------------------------------------
// Runner state
// ---------------------------------------------------------------------------

DrawStats s_drawStats{};
ui::FrameDamage s_frameDamage;
widgets::LuaVm s_vm;
bool s_running = false;
bool s_muniSelected = false;

// Load the chosen widget script: SD slug if one is selected, else the
// embedded copy. The source buffer can be freed immediately —
// luaL_loadbuffer copies it. Data must be injected before loadScript runs
// the chunk: scripts may read kami.data at top level.
void loadWidgetScript() {
    if (s_muniSelected) {
        s_vm.loadScript(kMuni5Widget, sizeof(kMuni5Widget)-1, "=muni-5");
        Serial.println("[lua-app] widget source: Muni 5 Fulton (built-in)");
        return;
    }
    bool fromSd = false;
    char path[64] = "";
    if (s_chosenSlug[0] != '\0') {
        snprintf(path, sizeof(path), "%s/%s/main.lua", kWidgetsDir, s_chosenSlug);
        char* buf = (char*)heap_caps_malloc(kMaxScriptBytes, MALLOC_CAP_SPIRAM);
        if (buf && hal::sd::mounted()) {
            char dpath[80];
            snprintf(dpath, sizeof(dpath), "%s/%s/data.json", kWidgetsDir, s_chosenSlug);
            const size_t dn = hal::sd::readFile(dpath, buf, kMaxDataBytes);
            if (dn > 0 && s_vm.setDataJson(buf, dn)) {
                Serial.printf("[lua-app] widget data: %s (%u B)\n", dpath, (unsigned)dn);
            }
            const size_t n = hal::sd::readFile(path, buf, kMaxScriptBytes);
            if (n > 0) {
                s_vm.loadScript(buf, n, "=widget");
                fromSd = true;
            }
        }
        if (buf) heap_caps_free(buf);
    }
    if (!fromSd) {
        s_vm.loadScript(kSampleWidget, strlen(kSampleWidget), "=counter");
    }
    Serial.printf("[lua-app] widget source: %s\n", fromSd ? path : "(embedded)");
}

// Built in the content coordinate space (280 x 240-kBarH); rendered offset
// below the status bar like the live widget, so nothing clips.
void buildFaultScene(ui::scene::Scene& scene, const char* msg) {
    const int16_t h = 240 - kBarH;
    scene.clear();
    scene.addRect("fault.bg", {0, 0, 280, h}, kUiBg);
    scene.addRect("fault.bar", {0, 40, 280, 40}, 0xF800 /* red */);
    scene.addLabel("fault.title", {0, 50, 280, 20}, "Widget error",
                   0xFFFF, 0xF800, 2, ui::scene::TextAlign::TopCenter);
    scene.addLabel("fault.msg", {12, 100, 256, 70}, msg ? msg : "unknown",
                   kUiFg, kUiBg, 1, ui::scene::TextAlign::TopLeft);
    scene.addLabel("fault.hint", {0, (int16_t)(h - 26), 280, 18}, "LB: back",
                   0x8410, kUiBg, 1, ui::scene::TextAlign::TopCenter);
}

// ---------------------------------------------------------------------------
// Runner screen
// ---------------------------------------------------------------------------

extern const ::scene::Screen kScreenPicker;

void startWidget() {
    s_running = true;
    if (!s_vm.isOpen()) {
        if (!s_vm.open(kMemCeilingBytes)) {
            Serial.printf("[lua-app] VM open failed: %s\n", s_vm.error());
        }
    }
    if (s_vm.isOpen() && !s_vm.faulted()) {
        loadWidgetScript();
        s_vm.callStart();
    }
}

void stopWidget() {
    s_frameDamage.release();
    if (s_vm.isOpen()) s_vm.callStop();
    s_vm.close();
    s_running = false;
}

extern const ::scene::Screen kScreenLua;

// Workflow transitions own the VM independently of presentation callbacks.
void showWidgetScreen(const ::scene::Screen* next) {
    stopWidget();
    if (next == &kScreenLua) startWidget();
    ::scene::switchTo(&app, next, next==&kScreenPicker ? ::scene::ScreenTransition::Back : ::scene::ScreenTransition::Forward);
}

void resumeWorkflow() {
    // Navigation resets to the picker when uncovered; system overlays retain
    // the runner and historically reload its chunk/start callback on dismissal.
    if (app.currentScreen == &kScreenLua) startWidget();
    else stopWidget();
}

void runnerEnter() { hal::display::markAllDirty(); }

void runnerTick(uint32_t now) {
    if (s_running && s_vm.isOpen() && !s_vm.faulted()) s_vm.callTick(now);
}

void runnerInput(hal::input::InputEvent ev) {
    using ET = hal::input::EventType;
    // LB is host-reserved back, so a buggy widget can never trap the user.
    if (ui::nav::isBackTap(ev)) {
        showWidgetScreen(&kScreenPicker);
        return;
    }
    if (!s_vm.isOpen() || s_vm.faulted()) return;
    if (ev.type == ET::Scroll && ev.delta != 0) {
        s_vm.callInput("scroll", ev.delta);
    } else if (ui::nav::isEnterTap(ev)) {
        s_vm.callInput("A", 0);
    }
}

bool runnerChord() {
    // LB+RB chord is host-reserved back: yield to NavHandler to pop the app.
    return false;
}

// Keep executing script draws and the existing renderer. Compare the actual
// framebuffer so overrides, clipping, overlays and partial scenes stay honest.
void renderContent(const ui::scene::Scene& scene, TFT_eSprite& canvas) {
    const int rows = canvas.height() - kBarH;
    const size_t rowBytes = size_t(canvas.width()) * sizeof(uint16_t);
    auto* pixels = static_cast<uint8_t*>(canvas.getPointer());
    if (canvas.getColorDepth() != 16 || !pixels || rows <= 0) {
        scene.render(canvas, 0, kBarH);
        hal::display::markAllDirty();
        return;
    }
    pixels += size_t(kBarH) * rowBytes;
    s_frameDamage.begin(pixels, rowBytes, rows);
    scene.render(canvas, 0, kBarH);
    const auto changed = s_frameDamage.finish(pixels);
    if (changed.count) hal::display::markDirty(kBarH + changed.first, changed.count);
}

void runnerDraw(TFT_eSprite& canvas) {
    if (!s_running) return;
    const uint32_t started = micros();
    ui::scene::Scene& scene = ui::scene::scratchScene();
    if (s_vm.faulted()) {
        buildFaultScene(scene, s_vm.error());
    } else {
        scene.clear();
        s_vm.buildScene(scene, millis(), 280, (int16_t)(240 - kBarH));
        if (s_vm.faulted()) buildFaultScene(scene, s_vm.error());
    }
    renderContent(scene, canvas);
    ++s_drawStats.frames;
    s_drawStats.elapsedUs += micros() - started;
}

void buildRunnerPlaceholder(ui::scene::Scene& scene, int16_t, int16_t) {
    // Static catalog/editor preview — the live scene is built per-frame by Lua.
    scene.clear();
    scene.addRect("bg", {0, 0, 280, 240}, kUiBg);
    scene.addLabel("title", {0, 100, 280, 24}, "Lua Widget", kUiFg, kUiBg, 2,
                   ui::scene::TextAlign::TopCenter);
}

const ::scene::Screen kScreenLua = {
    "Lua",
    runnerEnter,
    nullptr,
    runnerTick,
    runnerInput,
    runnerChord,
    runnerDraw,
    "run",
    buildRunnerPlaceholder,
};

// ---------------------------------------------------------------------------
// Loading screen — shown while the Kami agent generates a pushed widget.
// The agent POSTs widget_loading (-> we land here), then widget_ready (-> we
// sync just that widget and auto-launch the runner) or widget_error.
// ---------------------------------------------------------------------------

char s_loadingTitle[40] = "";
uint32_t s_loadingStartMs = 0;
char s_loadingErr[80] = "";
uint32_t s_loadingErrMs = 0;

void loadingEnter() {
    s_muniSelected = false;
    snprintf(s_loadingTitle, sizeof(s_loadingTitle), "%s", widgets::launch::title());
    s_loadingStartMs = millis();
    s_loadingErr[0] = '\0';
    s_loadingErrMs = 0;
    hal::display::markAllDirty();
}

void loadingExit() {}

void loadingTick(uint32_t now) {
    // The loading screen is passive (no button presses), so keep the shell's
    // idle timer from popping us back to the homescreen before widget_ready
    // arrives — generation can take 10-20s.
    services::notifyUserActivity();

    // Error already shown → linger ~2.2s then return to the picker.
    if (s_loadingErr[0]) {
        if ((uint32_t)(now - s_loadingErrMs) >= 2200u) {
            showWidgetScreen(&kScreenPicker);
        }
        return;
    }
    // Ready → pull just this widget, then auto-launch the runner.
    char id[40], slug[kSlugLen];
    if (widgets::launch::takeReady(id, sizeof(id), slug, sizeof(slug))) {
        char err[48] = "";
        if (widgets::sync::syncOne(id, slug, err, sizeof(err)) == 0) {
            snprintf(s_chosenSlug, sizeof(s_chosenSlug), "%s", slug);
            showWidgetScreen(&kScreenLua);
        } else {
            snprintf(s_loadingErr, sizeof(s_loadingErr), "%s", err);
            s_loadingErrMs = now;
            hal::display::markAllDirty();
        }
        return;
    }
    // Error push from the server.
    char msg[80];
    if (widgets::launch::takeError(msg, sizeof(msg))) {
        snprintf(s_loadingErr, sizeof(s_loadingErr), "%s", msg);
        s_loadingErrMs = now;
        hal::display::markAllDirty();
        return;
    }
    // Drawing runs every frame; content comparison notices each dot change.
}

void buildLoadingScene(ui::scene::Scene& scene, int16_t, int16_t) {
    const int16_t h = 240 - kBarH;
    scene.clear();
    scene.addRect("ld.bg", {0, 0, 280, h}, kUiBg);
    if (s_loadingErr[0]) {
        scene.addLabel("ld.title", {0, 40, 280, 24}, "Couldn't build it",
                       0xF800, kUiBg, 2, ui::scene::TextAlign::TopCenter);
        scene.addLabel("ld.msg", {12, 84, 256, 60}, s_loadingErr,
                       kUiMuted, kUiBg, 1, ui::scene::TextAlign::TopCenter);
        return;
    }
    // Animated "Building <title>" with 0-3 trailing dots (~400ms/step).
    const int dots = ((millis() - s_loadingStartMs) / 400u) % 4u;
    char line[64];
    snprintf(line, sizeof(line), "Building %s%s", s_loadingTitle,
             dots == 0 ? "" : dots == 1 ? "." : dots == 2 ? ".." : "...");
    scene.addLabel("ld.building", {0, 78, 280, 20}, "MAKING", kUiMuted, kUiBg, 1,
                   ui::scene::TextAlign::TopCenter);
    scene.addLabel("ld.title", {8, 100, 264, 30}, line, kUiFg, kUiBg, 2,
                   ui::scene::TextAlign::TopCenter);
    scene.addLabel("ld.hint", {0, (int16_t)(h - 26), 280, 16},
                   "Kami is on it   LB: cancel", kUiMuted, kUiBg, 1,
                   ui::scene::TextAlign::TopCenter);
}

void loadingDraw(TFT_eSprite& canvas) {
    const uint32_t started = micros();
    ui::scene::Scene& scene = ui::scene::scratchScene();
    buildLoadingScene(scene, canvas.width(), canvas.height());
    renderContent(scene, canvas);
    ++s_drawStats.frames;
    s_drawStats.elapsedUs += micros() - started;
}

void loadingInput(hal::input::InputEvent ev) {
    if (ui::nav::isBackTap(ev)) {
        showWidgetScreen(&kScreenPicker);  // cancel
    }
}

bool loadingChord() { return false; }

const ::scene::Screen kScreenLoading = {
    "Loading",
    loadingEnter,
    loadingExit,
    loadingTick,
    loadingInput,
    loadingChord,
    loadingDraw,
    "loading",
    buildLoadingScene,
};

// ---------------------------------------------------------------------------
// Picker screen
// ---------------------------------------------------------------------------

ui::ListItem pickerItemFn(int idx, void*) {
    ui::ListItem item;
    item.label = (idx >= 0 && idx < pickerCount()) ? pickerLabel(idx) : "";
    item.fg    = kUiFg;
    return item;
}

void applyPickerListLayout() {
    s_list.rowH = menu_layout::kMenuListRowH;
    s_list.rowPitch = menu_layout::kMenuListRowPitch;
    s_list.topPad = menu_layout::kMenuListTopPad;
    s_list.leftX = menu_layout::kMenuListLeftX;
    s_list.scrollbarW = menu_layout::kMenuListScrollbarW;
    s_list.scrollbarPad = menu_layout::kMenuListScrollbarPad;
    s_list.scrollbarRight = menu_layout::kMenuListScrollbarRight;
}

void pickerEnter() {
    rescanWidgets();
    Serial.printf("[lua-app] picker: %d SD widget(s) + built-in\n", s_sdCount);
    s_needRedraw = true;
    hal::display::markAllDirty();
}

void pickerExit() {}

void pickerTick(uint32_t) {
    if (widgets::sync::takeSyncNotification()) {
        rescanWidgets();
        snprintf(s_syncMsg, sizeof(s_syncMsg), "auto-synced %d widget%s",
                 s_sdCount, s_sdCount == 1 ? "" : "s");
        s_needRedraw = true;
        hal::display::markAllDirty();
    }
}

void pickerInput(hal::input::InputEvent ev) {
    using ET = hal::input::EventType;

    if (ev.type == ET::Scroll && ev.delta != 0) {
        s_list.totalItems = pickerCount();
        s_list.selected   = s_sel;
        s_list.scroll(ev.delta);
        if (s_list.selected != s_sel) {
            s_sel = s_list.selected;
            s_needRedraw = true;
            hal::display::markAllDirty();
        }
        return;
    }

    if (ui::nav::isEnterTap(ev)) {
        if (s_sel == s_sdCount + 2) {
            // Sync action. Blocks the UI task for the HTTP round-trips —
            // acceptable for a manual dev-triggered pull of a few KB.
            runSync();
            s_needRedraw = true;
            hal::display::markAllDirty();
            return;
        }
        s_muniSelected = s_sel == s_sdCount + 1;
        if (s_sel >= 0 && s_sel < s_sdCount) {
            snprintf(s_chosenSlug, sizeof(s_chosenSlug), "%s",
                     s_slugs + s_sel * kSlugLen);
        } else {
            s_chosenSlug[0] = '\0';  // built-in counter
        }
        showWidgetScreen(&kScreenLua);
        return;
    }

    if (ui::nav::isBackTap(ev)) {
        NavHandler::instance().pop();
        return;
    }
}

bool pickerChord() {
    // Yield to NavHandler to pop back to the menu.
    return false;
}

void buildPickerScene(ui::scene::Scene& scene, int16_t, int16_t) {
    // Content bg starts below the status bar so the bar (battery/wifi/clock)
    // stays visible — same offset as the native app menu. The list area
    // (kMenuListAreaY) is already below the bar. Sync status sits at the bottom.
    scene.clear();
    scene.addRect("bg", {0, kBarH, 280, (int16_t)(240 - kBarH)}, kUiBg);
    if (s_syncMsg[0]) {
        scene.addLabel("sync", {0, 222, 280, 14}, s_syncMsg, kUiMuted, kUiBg, 1,
                       ui::scene::TextAlign::TopCenter);
    }
    applyPickerListLayout();
    s_list.totalItems = pickerCount();
    s_list.selected   = s_sel;
    s_list.buildScene(scene,
                      menu_layout::kMenuListAreaX,
                      menu_layout::kMenuListAreaY,
                      menu_layout::kMenuListAreaW,
                      menu_layout::kMenuListAreaH - (s_syncMsg[0] ? 16 : 0),
                      pickerItemFn, nullptr,
                      kUiSelBg, kUiSelFg, kUiBg, kUiMuted, kUiFg);
}

void pickerDraw(TFT_eSprite& canvas) {
    if (!s_needRedraw) return;
    s_needRedraw = false;
    ui::scene::Scene& scene = ui::scene::scratchScene();
    buildPickerScene(scene, canvas.width(), canvas.height());
    scene.render(canvas);
    NavHandler::instance().invalidateChrome();  // keep the status bar painted on top
    hal::display::markAllDirty();
}

const ::scene::Screen kScreenPicker = {
    "Widgets",
    pickerEnter,
    pickerExit,
    pickerTick,
    pickerInput,
    pickerChord,
    pickerDraw,
    "picker",
    buildPickerScene,
};

}  // namespace

static const ::scene::Screen* const kScreens[] = {&kScreenPicker, &kScreenLua, &kScreenLoading};
App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS(
    "Lua Widget", "lua", &kScreenPicker, nullptr, resumeWorkflow, stopWidget,
    "src/widgets/app.cpp", kScreens, nullptr, nullptr);

// Entry point for the push-driven launch flow: the ui_task main loop calls this
// when the server pushes widget_loading, after ensuring this app is on the nav
// stack. Switches to the loading screen, which then auto-launches on ready.
DrawStats drawStats() { return s_drawStats; }

void showLoading() {
    showWidgetScreen(&kScreenLoading);
}

}  // namespace widgets
