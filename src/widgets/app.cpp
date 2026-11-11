#include <new>
#ifdef ORIGAMI_NATIVE_APPS
#include "control/native_apps.h"
#endif
/**
 * @file lua_app.cpp
 * @brief Host for sandboxed Lua widgets (Phase 2b).
 *
 * Three screens: loading, a picker for persistent SD/flash and legacy SD apps
 * (plus built-ins), and a runner that drives one LuaVm for
 * the chosen script. App workflow transitions own VM startup and teardown;
 * screen entry only repaints. App resume explicitly retains the established
 * reload behavior after system overlays.
 */

#include "widgets/app.h"
#include "services/vibration.h"
#include "apps/app_store_app.h"
#include "apps/steps_app.h"
#include "assets/menu_icons.h"
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
#include "widgets/storage.h"
#include "services/custom_apps.h"
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

constexpr int kMaxWidgets        = services::custom_apps::kCatalogEntries;
constexpr int kSlugLen           = 32;

// The system status bar (battery/wifi/clock) is always visible: widgets and the
// picker render in the content area BELOW it, and the host chrome is repainted
// on top each frame. Mirrors the native menu's content offset.
constexpr int16_t kBarH = menu_layout::kMenuContentBgY;  // follows the system status strip

// ---------------------------------------------------------------------------
// Picker state
// ---------------------------------------------------------------------------

namespace custom=services::custom_apps;
custom::Catalog* s_catalog=nullptr; // UI-owned metadata belongs in PSRAM, not TLS/DMA internal RAM.
uint8_t* s_icons=nullptr; // UI-owned 4-bit masks in PSRAM; never executable code.
custom::Identity s_selected;
custom::Package s_currentPackage, s_readyPackage;
bool s_externalSelected=false;
bool s_loadRequested=false;
bool s_nativePreparing=false;
uint32_t s_catalogJob=0,s_launchJob=0,s_syncJob=0;
bool s_catalogWanted=false;
char s_remoteId[40]={},s_remoteSlug[kSlugLen]={};
void releaseLoadedScript(){s_currentPackage.reset();}
void cancelLoading(){s_nativePreparing=false;s_loadRequested=false;s_remoteId[0]=0;if(s_launchJob)storage::abandon(s_launchJob);s_launchJob=0;s_readyPackage.reset();}
void closeAppWork(){s_catalogWanted=false;cancelLoading();if(s_catalogJob)storage::abandon(s_catalogJob);s_catalogJob=0;
    if(s_syncJob)storage::abandon(s_syncJob);s_syncJob=0;}
int  s_sdCount = 0;          // external apps in the bounded catalog
int  s_sel = 0;
bool s_needRedraw = false;
ui::ListView s_list;

// Result of the last "sync from kami" action, shown under the picker title.
char s_syncMsg[40] = "";

// Install first, then external apps, built-ins, Steps and account sync.
int pickerCount() { return s_sdCount + 5; }
const char* pickerLabel(int idx) {
    if(idx==0)return "App Downloader";
    if(idx>0 && idx<=s_sdCount)return s_catalog->entries[idx-1].name;
    if(idx==s_sdCount+1)return "Counter";
    if(idx==s_sdCount+2)return "Muni 5 Fulton";
    if(idx==s_sdCount+3)return "Steps";
    return "Sync apps";
}

void rescanWidgets() {
    if(!s_catalog){
        void* memory=heap_caps_malloc(sizeof(custom::Catalog),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!memory){snprintf(s_syncMsg,sizeof(s_syncMsg),"app list: no memory");s_needRedraw=true;return;}
        s_catalog=new(memory) custom::Catalog{};
    }
    s_catalogWanted=true;
    if(!s_catalogJob)s_catalogJob=storage::request(storage::Operation::Catalog);
    if(s_catalogJob)s_catalogWanted=false;
}

void runSync() {
    if(!s_syncJob)s_syncJob=storage::request(storage::Operation::Sync);
    snprintf(s_syncMsg,sizeof(s_syncMsg),"%s",s_syncJob?"syncing...":"storage busy; retry");
}

// ---------------------------------------------------------------------------
// Runner state
// ---------------------------------------------------------------------------

DrawStats s_drawStats{};
ui::FrameDamage s_frameDamage;
widgets::LuaVm s_vm;
bool s_running = false;
bool s_muniSelected = false;

// Load the owned external snapshot or selected built-in. Keep the snapshot
// for overlay reloads. Data must be injected before loadScript runs
// the chunk: scripts may read kami.data at top level.
void loadWidgetScript() {
    if(s_readyPackage.bytes){s_currentPackage.reset();s_currentPackage.bytes=s_readyPackage.bytes;s_currentPackage.size=s_readyPackage.size;
        s_readyPackage.bytes=nullptr;s_readyPackage.size=0;}
    if (s_muniSelected) {
        s_vm.loadScript(kMuni5Widget, sizeof(kMuni5Widget)-1, "=muni-5");
        return;
    }
    if(s_externalSelected){
        custom::View view;auto result=custom::inspect(s_currentPackage.bytes,s_currentPackage.size,view);
        if(result!=custom::Result::Ok){s_vm.fail(custom::resultName(result));return;}
        if(view.dataBytes && !s_vm.setDataJson(view.data,view.dataBytes)){s_vm.fail("invalid app data");return;}
        s_vm.loadScript(view.source,view.sourceBytes,"=widget");return;
    }
    s_vm.loadScript(kSampleWidget, strlen(kSampleWidget), "=counter");
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
    releaseLoadedScript();
    s_running = false;
}

extern const ::scene::Screen kScreenLua;
extern const ::scene::Screen kScreenLoading;

// Workflow transitions own the VM independently of presentation callbacks.
void showWidgetScreen(const ::scene::Screen* next) {
    if(next!=&kScreenLua && next!=&kScreenLoading)cancelLoading();
    stopWidget();
    if (next == &kScreenLua) startWidget();
    ::scene::switchTo(&app, next, next==&kScreenPicker ? ::scene::ScreenTransition::Back : ::scene::ScreenTransition::Forward);
}

void resumeWorkflow() {
    // Navigation resets to the picker when uncovered; system overlays retain
    // the runner and historically reload its chunk/start callback on dismissal.
    if (app.currentScreen == &kScreenLua) startWidget();
    else {
        if(app.currentScreen==&kScreenPicker)cancelLoading();
        stopWidget();
    }
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
    scene.addLabel("title", {0, 100, 280, 24}, "Custom Apps", kUiFg, kUiBg, 2,
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
    // Existing worker loads owned bytes; no filesystem or HTTP in this tick.
    if(s_loadRequested&&!s_launchJob){s_launchJob=storage::request(storage::Operation::Load,s_selected);if(s_launchJob)s_loadRequested=false;}
    if(s_launchJob){custom::Result result;
        if(storage::take(s_launchJob,result,&s_readyPackage,nullptr,&s_selected)){
            s_launchJob=0;
            if(result==custom::Result::Ok){
#ifdef ORIGAMI_NATIVE_APPS
                if(s_nativePreparing){
                    s_nativePreparing=false;s_readyPackage.reset();showWidgetScreen(&kScreenPicker);
                    if(!control::native_apps::openPrepared()){snprintf(s_syncMsg,sizeof(s_syncMsg),"Native app could not start");hal::display::markAllDirty();}
                    return;
                }
                custom::View view;
                if(custom::inspect(s_readyPackage.bytes,s_readyPackage.size,view)==custom::Result::Ok && view.native){
                    s_launchJob=storage::request(storage::Operation::PrepareNative,s_selected,s_readyPackage.bytes,s_readyPackage.size);
                    s_readyPackage.reset();s_nativePreparing=s_launchJob!=0;
                    if(!s_launchJob){snprintf(s_loadingErr,sizeof(s_loadingErr),"Native loader busy");s_loadingErrMs=now;hal::display::markAllDirty();}
                    return;
                }
#endif
                s_muniSelected=false;s_externalSelected=true;showWidgetScreen(&kScreenLua);
            }
            else {s_readyPackage.reset();snprintf(s_loadingErr,sizeof(s_loadingErr),"%s",custom::resultName(result));s_loadingErrMs=now;hal::display::markAllDirty();}
        }
        return;
    }
    if(!s_remoteId[0])widgets::launch::takeReady(s_remoteId,sizeof(s_remoteId),s_remoteSlug,sizeof(s_remoteSlug));
    if(s_remoteId[0]){
        s_launchJob=storage::request(storage::Operation::Sync,custom::Identity{},nullptr,0,s_remoteId,s_remoteSlug);
        if(s_launchJob)s_remoteId[0]=0;
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
    namespace icons=assets::menu_icons;
    item.icon=idx==0?icons::kInstall:idx==s_sdCount+3?icons::kSteps:idx==s_sdCount+4?icons::kTools:icons::kSdCard;
    if(idx>0 && idx<=s_sdCount && s_catalog->entries[idx-1].native){
        const auto& entry=s_catalog->entries[idx-1];
        item.icon=entry.hasIcon&&s_icons?s_icons+(idx-1)*200:icons::kGames;
        if(!entry.hasIcon){
            const char* id=entry.identity.id;
            if(!strcmp(id,"pong"))item.icon=icons::kPong;
            else if(!strcmp(id,"tennis"))item.icon=icons::kTennis;
            else if(!strcmp(id,"bowling"))item.icon=icons::kBowling;
            else if(!strcmp(id,"golf"))item.icon=icons::kGolf;
            else if(!strcmp(id,"tetris"))item.icon=icons::kBlocks;
            else if(!strcmp(id,"t-rex"))item.icon=icons::kDinosaur;
            else if(!strcmp(id,"doodle-jump"))item.icon=icons::kJump;
            else if(!strcmp(id,"dumb-flappy-bird"))item.icon=icons::kBird;
        }
    }
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

// Keep focus on the same app (or built-in action) when an async scan reorders rows.
int catalogSelection(const custom::Catalog& catalog,const custom::Identity* selected,int builtinRow) {
    if(!selected)return builtinRow<0?0:int(catalog.count)+builtinRow;
    for(size_t i=0;i<catalog.count;++i){const auto& id=catalog.entries[i].identity;
        if(id.medium==selected->medium && id.legacy==selected->legacy &&
           !strcmp(id.origin,selected->origin) && !strcmp(id.id,selected->id))return int(i)+1;
    }
    return 0;
}

void pickerTick(uint32_t) {
    custom::Result result;
    if(s_catalogWanted && !s_catalogJob && !s_syncJob)rescanWidgets();
    custom::Identity focused;
    const bool focusedExternal=s_sel>0 && s_sel<=s_sdCount;
    const int builtinRow=s_sel==0?-1:s_sel-s_sdCount;
    if(focusedExternal)focused=s_catalog->entries[s_sel-1].identity;
    if(s_catalogJob && storage::take(s_catalogJob,result,nullptr,s_catalog)){
        s_sel=catalogSelection(*s_catalog,focusedExternal?&focused:nullptr,builtinRow);
        s_catalogJob=0;s_sdCount=int(s_catalog->count);
        bool needsIcons=false;for(int i=0;i<s_sdCount;++i)needsIcons|=s_catalog->entries[i].hasIcon;
        if(needsIcons&&!s_icons)s_icons=static_cast<uint8_t*>(heap_caps_malloc(kMaxWidgets*200,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
        for(int i=0;i<s_sdCount;++i){const auto& e=s_catalog->entries[i];
            if(s_icons)for(unsigned j=0;j<200;++j){const unsigned pixel=j*2;
                const bool hi=e.icon[pixel/8]&(0x80>>(pixel%8));
                const bool lo=e.icon[(pixel+1)/8]&(0x80>>((pixel+1)%8));
                s_icons[i*200+j]=(hi?0xf0:0)|(lo?0x0f:0);
            }
        }
        if(s_sel>=pickerCount())s_sel=0;
        if(result!=custom::Result::Ok || s_catalog->overflow)snprintf(s_syncMsg,sizeof(s_syncMsg),"%s",s_catalog->overflow?"more apps than picker capacity":custom::resultName(result));
        s_needRedraw=true;hal::display::markAllDirty();
    }
    if(s_syncJob && storage::take(s_syncJob,result)){
        s_syncJob=0;snprintf(s_syncMsg,sizeof(s_syncMsg),"sync: %s",custom::resultName(result));rescanWidgets();s_needRedraw=true;hal::display::markAllDirty();
    }
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
        if(s_sel==s_sdCount+3){closeAppWork();NavHandler::instance().push(&apps::steps::app);return;}
        if(s_sel==0){closeAppWork();NavHandler::instance().push(&apps::app_store::app);return;}
        if (s_sel == s_sdCount + 4) {
            // Queue the existing worker; keep input and presentation responsive.
            runSync();
            s_needRedraw = true;
            hal::display::markAllDirty();
            return;
        }
        closeAppWork();
        s_muniSelected = s_sel == s_sdCount + 2;
        s_externalSelected=s_sel>0&&s_sel<=s_sdCount;
        if(s_externalSelected){
            s_selected=s_catalog->entries[s_sel-1].identity;
            showWidgetScreen(&kScreenLoading);
            s_loadRequested=true;
            snprintf(s_loadingTitle,sizeof(s_loadingTitle),"%s",s_catalog->entries[s_sel-1].name);
        } else showWidgetScreen(&kScreenLua);
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
    "Custom Apps",
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

static void exitWorkflow(){closeAppWork();stopWidget();heap_caps_free(s_icons);s_icons=nullptr;heap_caps_free(s_catalog);s_catalog=nullptr;s_sdCount=0;}

static const ::scene::Screen* const kScreens[] = {&kScreenPicker, &kScreenLua, &kScreenLoading};
App app = {"Custom Apps", &kScreenPicker, nullptr, nullptr, exitWorkflow, "lua",
    "src/widgets/app.cpp", kScreens, uint8_t(sizeof(kScreens)/sizeof(kScreens[0])),
    nullptr, nullptr, nullptr, resumeWorkflow, nullptr, services::vibration::stopWidgetCue};

// Entry point for the push-driven launch flow: the ui_task main loop calls this
// when the server pushes widget_loading, after ensuring this app is on the nav
// stack. Switches to the loading screen, which then auto-launches on ready.
DrawStats drawStats() { return s_drawStats; }

void showLoading() {
    closeAppWork();
    showWidgetScreen(&kScreenLoading);
}

}  // namespace widgets
