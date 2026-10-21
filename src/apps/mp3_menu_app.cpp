#include "services/vibration.h"
#include "config/screen_chrome_layout.h"
/**
 * @file mp3_menu_app.cpp
 * @brief MP3 player — file browser + now-playing screen.
 *
 * All decode / buffer / task / ID3 logic lives in services::audio::Mp3Player.
 * This app owns: file list, browse index, canvas sub-state, UI drawing, input.
 *
 * Sub-state and canvas pipeline
 * ------------------------------
 *   MP3_BROWSER  — canvas sprite pipeline (tear-free list scrolling)
 *   MP3_PLAYING  — optimized-render bypass (same as camera viewfinder; avoids
 *                  134 KB canvas flush contending with SD reads in decode task)
 */

#include "ui/coverage_surface.h"
#include "ui/direct_bands.h"
#include "apps/mp3_menu_app.h"

#include <Arduino.h>
#include "app.h"
#include <cstring>

#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "services/mp3_player.h"
#include "services/music_power.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "services/audio_library.h"
#include "scene/screen.h"
#include "ui/draw.h"
#include "ui/music_kami_view.h"
#include "core/board_time.h"
#include "ui/morning_style.h"
#include "ui/list_view.h"
#include "ui/nav_controls.h"
#include "ui/scene.h"
#include "nav_handler.h"
#include "ui/source_layout.h"

namespace apps {
namespace mp3_menu {

// ---------------------------------------------------------------------------
// File list + playback state
// ---------------------------------------------------------------------------

static services::audio::Library s_library{services::audio::Library::Kind::Music};
static int s_lastFileCount = 0;  // Keep the last scan in inactive diagnostic status.
static int  mp3BrowseIdx = 0;
static int  mp3TrackIdx  = -1;
static char s_musicDir[services::audio::Library::kPathSize] = "/music";
static bool hasParent() { return strcmp(s_musicDir, "/music") != 0; }
static int browserCount() { return s_library.count() + (hasParent() ? 1 : 0); }

static void loadFolder() {
  s_lastFileCount = s_library.load(s_musicDir);
  mp3BrowseIdx = 0;
}
static void parentFolder() {
  if (!hasParent()) return;
  services::vibration::navigationFeedback(services::vibration::Cue::Back);
  char* slash = strrchr(s_musicDir, '/');
  if (slash) *slash = 0;
  loadFolder();
}

static services::audio::Mp3Player s_player;

static services::audio::MusicPower s_power;
enum Mp3SubState { MP3_BROWSER, MP3_PLAYING, MP3_SETTINGS };
static Mp3SubState s_settingsReturn = MP3_BROWSER;
static int s_setting = 0;
static bool s_saveFailed = false;
static bool s_settingsDirty = true;
static Mp3SubState mp3Sub = MP3_BROWSER;
static bool s_appActive = false;

extern const ::scene::Screen kScreenMusicBrowser;
extern const ::scene::Screen kScreenMusicNowPlaying;
extern const ::scene::Screen kScreenMusicSettings;

static void showBrowser();
static void showNowPlaying();
static void showSettings();
static void closeSettings();

static inline bool mp3UsesDirectPanel() { return mp3Sub == MP3_PLAYING; }

static bool s_browserNeedsRedraw = false;

// ---------------------------------------------------------------------------
// Canvas pipeline toggle
// ---------------------------------------------------------------------------
static void mp3SetSubState(Mp3SubState target) {
  if (target == mp3Sub) return;
  const Mp3SubState prev = mp3Sub;
  mp3Sub = target;
  if (target == MP3_PLAYING && prev != MP3_PLAYING) {
    hal::display::enterOptimizedRender();
  } else if (target != MP3_PLAYING && prev == MP3_PLAYING) {
    hal::display::exitOptimizedRender();
    NavHandler::instance().invalidateChrome();
    s_browserNeedsRedraw = true;
  }
}

static ui::ListView s_browser;
static uint32_t s_volumeUntil = 0;
static bool s_volumeVisible = false;
static uint32_t mp3LastProgressMs = 0;
static uint32_t s_animationStartMs = 0;
static uint32_t s_lastAnimationMs = 0;
static uint32_t s_elapsedSecond = UINT32_MAX;
static char s_clock[8] = {};

static music_view::State playerViewState() {
  const auto& info = s_player.trackInfo();
  return {info.title, info.artist, s_player.elapsedMs()/1000,
          info.durationMs/1000, s_player.isPlaying(), s_player.volume(),
          s_volumeVisible, s_clock, 5};
}

// A small sprite buffers each dirty band. Only its cropped pixels cross SPI;
// there is no whole-screen repaint in the playback tick.
static void drawPlayerRegion(int x,int y,int w,int h) {
  if (s_power.dark()) return;
  const auto state=playerViewState();
  const unsigned phase=millis()-s_animationStartMs;
  ui::drawDirectBands(x,y,w,h,[&](ui::BandSurface& surface) {
    music_kami::render(surface,state,phase,true,true);
  });
}

// The scene inspector uses the same renderer, rasterized into a 4-bit mask.

static void buildNowPlayingScene(ui::scene::Scene& graph,int16_t,int16_t) {
  static uint8_t* pixels=nullptr;
  if(!pixels)pixels=(uint8_t*)ps_malloc(280*240/2);
  graph.clear();
  if(!pixels)return;
  ui::CoverageSurface surface{pixels,280,240};
  music_kami::render(surface,playerViewState(),millis()-s_animationStartMs,true,true);
  graph.addBitmap("music.player",{0,0,280,240},pixels,280,240,kUiFg,kUiBg,0,4);
}

static void mp3DrawNowPlaying() {
  s_animationStartMs=millis();
  s_lastAnimationMs=s_animationStartMs;
  core::time::formatClockHHMM(s_clock,sizeof(s_clock));
  s_volumeVisible=false;
  s_elapsedSecond=s_player.elapsedMs()/1000;
  drawPlayerRegion(0,0,280,240);
}

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------
static ui::ListItem mp3BrowserItemFn(int idx, void*) {
  static char s_name[48];
  if (hasParent() && idx == 0) {
    ui::ListItem item; item.label = ".. / Back"; item.fg = kUiMuted; return item;
  }
  idx -= hasParent() ? 1 : 0;
  s_library.name(idx, s_name, sizeof(s_name));
  if (s_library.isDirectory(idx)) strlcat(s_name, "/", sizeof(s_name));
  ui::ListItem item;
  item.label = s_name;
  item.fg    = kUiFg;
  return item;
}

static void buildBrowserScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  ui::scene::Builder b(scene, width, height);
  b.clearContent(kUiBg);
  b.statusLine("music.folder", s_musicDir, kUiMuted);
  if (browserCount() == 0) {
    b.emptyState("mp3.empty", "No music or folders");
  } else {
    s_browser.totalItems = browserCount();
    s_browser.selected   = mp3BrowseIdx;
    s_browser.buildScene(scene, 0, b.curY(), width, height - b.curY(),
                         mp3BrowserItemFn, nullptr,
                         kUiSelBg, kUiSelFg, kUiBg, kUiMuted, kUiFg);
  }
  ui::scene::applySourceLayout(scene, "music", "browser");
}

static void mp3DrawBrowser(TFT_eSprite& canvas) {
  ui::scene::Scene& scene = ui::scene::scratchScene();
  buildBrowserScene(scene, canvas.width(), canvas.height());
  scene.render(canvas);
  hal::display::markAllDirty();
  s_browserNeedsRedraw = false;
}

// ---------------------------------------------------------------------------
// Track lifecycle — thin wrappers so callers don't touch s_player directly
// ---------------------------------------------------------------------------

static bool mp3StartTrack(int trackIdx) {
  if (trackIdx < s_library.directoryCount() || trackIdx >= s_library.count()) return false;
  if (!s_player.ready()) {
    Serial.println("[mp3] Cannot play: decode buffers missing");
    return false;
  }
  mp3TrackIdx = trackIdx;
  if (!s_player.startTrack(s_library[trackIdx])) {
    mp3TrackIdx = -1;
    return false;
  }
  mp3LastProgressMs = 0;
  if (mp3Sub != MP3_SETTINGS) showNowPlaying();
  return true;
}

static void mp3PausePlayback() {
  s_player.pause();
  drawPlayerRegion(238,48,24,26);
  drawPlayerRegion(20,40,72,56);
}

static void mp3ResumePlayback() {
  s_player.resume();
  drawPlayerRegion(238,48,24,26);
  drawPlayerRegion(20,40,72,56);
}

static void mp3PlayNext() {
  if(s_library.count()<=s_library.directoryCount())return;
  Serial.printf("[mp3] NEXT (from track %d)\n", mp3TrackIdx);
  if (!s_player.stopTrack()) return;
  int next = s_library.directoryCount() + (mp3TrackIdx - s_library.directoryCount() + 1) % (s_library.count() - s_library.directoryCount());
  if (!mp3StartTrack(next)) {
    Serial.printf("[mp3] NEXT FAILED -> browser\n");
    showBrowser();
  }
}

static void mp3PlayPrev() {
  if(s_library.count()<=s_library.directoryCount())return;
  Serial.printf("[mp3] PREVIOUS (from track %d)\n",mp3TrackIdx);
  if (!s_player.stopTrack()) return;
  int tracks = s_library.count() - s_library.directoryCount();
  int prev = s_library.directoryCount() + (mp3TrackIdx - s_library.directoryCount() - 1 + tracks) % tracks;
  if (!mp3StartTrack(prev)) {
    showBrowser();
  }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// App lifecycle
// ---------------------------------------------------------------------------

static void mp3Enter() {
  s_appActive = true;
  s_power.load();
  s_power.activity(millis());
  ui::showAppOpeningSplash("Music");

  mp3TrackIdx = -1;
  mp3Sub      = MP3_BROWSER;
  mp3BrowseIdx   = 0;
  s_browser.selected = 0;
  s_browser.scrollFirst = 0;
  s_volumeVisible=false;
  mp3LastProgressMs=0;

  // The playback service owns codec, buffers and decoder-worker lifetime.
  s_player.openSession();

  strlcpy(s_musicDir, "/music", sizeof(s_musicDir));
  loadFolder();
  s_browserNeedsRedraw = true;
  hal::display::markAllDirty();

}

static void mp3Exit() {
  s_power.leave();
  s_player.closeSession();
  s_library.close();
  mp3SetSubState(MP3_BROWSER);
  s_appActive = false;
}

void tick() {
  uint32_t now = millis();

  if (mp3Sub == MP3_BROWSER) { s_power.tick(now, false); return; }

  if (s_player.consumeWantsNextTrack()) {
    Serial.println("[mp3] EOT -> next");
    if (s_library.count() > 0) mp3PlayNext();
  }

  s_power.tick(now, mp3Sub == MP3_PLAYING && s_player.isPlaying());
  if(mp3Sub!=MP3_PLAYING || s_power.dark())return;
  if(now-s_lastAnimationMs>=80) {
    s_lastAnimationMs=now;
    if(s_player.isPlaying())drawPlayerRegion(20,40,72,56);
    if(strlen(s_player.trackInfo().title)>16)drawPlayerRegion(24,132,200,18);
  }
  if(s_volumeVisible && (int32_t)(now-s_volumeUntil)>=0) {
    s_volumeVisible=false;
    drawPlayerRegion(245,84,8,64);
  }
  if(now-mp3LastProgressMs>=250) {
    mp3LastProgressMs=now;
    const uint32_t elapsed=s_player.elapsedMs()/1000;
    if(elapsed!=s_elapsedSecond) {
      s_elapsedSecond=elapsed;
      drawPlayerRegion(26,184,228,40);
    }
    char clock[8]; core::time::formatClockHHMM(clock,sizeof(clock));
    if(strcmp(clock,s_clock)!=0) {
      strncpy(s_clock,clock,sizeof(s_clock));
      drawPlayerRegion(104,12,72,24);
    }
  }
}

void onInput(hal::input::InputEvent ev) {
  using ET = hal::input::EventType;
  // Consume the whole wake gesture, including the release-generated tap.
  if (s_power.dark()) {
    s_power.activity(millis());
    hal::input::suppressUntilButtonsReleased();
    mp3DrawNowPlaying();
    return;
  }
  s_power.activity(millis());
  if (mp3Sub == MP3_SETTINGS) {
    if (ev.type == ET::Scroll && ev.delta) {
      s_setting = (s_setting + 1) % 2;
    } else if (ui::nav::isBackTap(ev) || ev.type == ET::ButtonB_Tap) {
      closeSettings(); return;
    } else if (ui::nav::isEnterTap(ev)) {
      if (s_setting == 0) s_saveFailed = !s_power.setEnabled(!s_power.enabled());
      else { closeSettings(); return; }
    }
    s_settingsDirty = true;
    hal::display::markAllDirty();
    return;
  }

  if (ev.type == ET::Scroll && ev.delta != 0) {
    if (mp3Sub == MP3_BROWSER && browserCount() > 0) {
      s_browser.scroll(ev.delta);
      mp3BrowseIdx = s_browser.selected;
      s_browserNeedsRedraw = true;
      hal::display::markAllDirty();
    } else if (mp3Sub == MP3_PLAYING) {
      int v = s_player.volume() - ev.delta * 5;
      if (v < 0) v = 0;
      if (v > 100) v = 100;
      s_player.setVolume(v);
      s_volumeVisible=true;
      s_volumeUntil=millis()+1500;
      drawPlayerRegion(245,84,8,64);
    }
    return;
  }

  if (ui::nav::isEnterTap(ev) && mp3Sub == MP3_BROWSER) {
    const int index = mp3BrowseIdx - (hasParent() ? 1 : 0);
    if (index < 0) parentFolder();
    else if (index < s_library.count() && s_library.isDirectory(index)) {
      strlcpy(s_musicDir, s_library[index], sizeof(s_musicDir));
      loadFolder();
    } else if (index < s_library.count() && !mp3StartTrack(index)) Serial.println("[mp3] Play failed");
    s_browser.selected = mp3BrowseIdx; s_browser.scrollFirst = 0;
    s_browserNeedsRedraw = true; hal::display::markAllDirty();
    return;
  }

  if (ev.type == ET::ButtonC_Tap) {
    if (mp3Sub == MP3_PLAYING) {
      if (s_player.isPlaying()) mp3PausePlayback();
      else if (s_player.isOpen()) mp3ResumePlayback();

    }
    return;
  }

  if (ui::nav::isBackTap(ev)) {
    if (mp3Sub == MP3_PLAYING) mp3PlayPrev();
    else if (hasParent()) {
      parentFolder(); s_browser.selected=0; s_browser.scrollFirst=0;
      s_browserNeedsRedraw=true; hal::display::markAllDirty();
    } else NavHandler::instance().pop();
    return;
  }

  if (ev.type == ET::ButtonB_Tap) {
    if (mp3Sub == MP3_PLAYING) mp3PlayNext();
    else if (mp3Sub == MP3_BROWSER) showSettings();
    return;
  }
}

void onDraw(TFT_eSprite& canvas) {
  if (mp3Sub != MP3_BROWSER) return;
  if (!s_browserNeedsRedraw) return;
  mp3DrawBrowser(canvas);
}

void enter()   { mp3Enter(); }
void exitApp() { mp3Exit();  }

bool onChord() {
  if (s_power.dark()) {
    s_power.activity(millis());
    hal::input::suppressUntilButtonsReleased();
    mp3DrawNowPlaying(); return true;
  }
  if (mp3Sub == MP3_SETTINGS) { closeSettings(); return true; }
  if (mp3Sub == MP3_BROWSER && hasParent()) {
    parentFolder(); s_browser.selected=0; s_browser.scrollFirst=0;
    s_browserNeedsRedraw=true; hal::display::markAllDirty(); return true;
  }
  if (mp3Sub == MP3_PLAYING) {
    if (s_player.stopTrack()) showBrowser();
    return true;
  }
  return false;
}

static void enterBrowserScreen() {
  s_power.leave();
  mp3SetSubState(MP3_BROWSER);
  s_browserNeedsRedraw = true;
  hal::display::markAllDirty();
}

static void enterNowPlayingScreen() {
  mp3SetSubState(MP3_PLAYING);
  mp3DrawNowPlaying();
}

static void screenTick(uint32_t) {
  tick();
}

const ::scene::Screen kScreenMusicBrowser = {
    "Browser",
    enterBrowserScreen,
    nullptr,
    screenTick,
    onInput,
    onChord,
    onDraw,
    "browser",
    buildBrowserScene,
    &screen_chrome_layout::kMusicBrowserTopBarColor,
};

const ::scene::Screen kScreenMusicNowPlaying = {
    "Now Playing",
    enterNowPlayingScreen,
    nullptr,
    screenTick,
    onInput,
    onChord,
    nullptr,
    "now-playing",
    buildNowPlayingScene,
    &screen_chrome_layout::kMusicNowPlayingTopBarColor,
};

static void showBrowser() {
  if (app.currentScreen == &kScreenMusicBrowser) {
    enterBrowserScreen();
    return;
  }
  ::scene::switchTo(&app, &kScreenMusicBrowser, ::scene::ScreenTransition::Back);
}

static void showNowPlaying() {
  if (app.currentScreen == &kScreenMusicNowPlaying) {
    enterNowPlayingScreen();
    return;
  }
  ::scene::switchTo(&app, &kScreenMusicNowPlaying);
}

static void buildSettingsScene(ui::scene::Scene& graph, int16_t width, int16_t height) {
  graph.clear();
  ui::scene::Builder b(graph, width, height);
  b.clearContent(kUiBg);
  b.panelHeader("Music settings");
  b.onOffRow("music.low-power", "Low power", s_power.enabled(), s_setting == 0);
  b.settingRow("music.return", "Back", nullptr, s_setting == 1);
  b.statusLine("music.power-help", s_saveFailed ? "Could not save. Try again." : "Screen off after 15s playing", kUiMuted);
  b.statusLine("music.wifi-help", "Wi-Fi off when safe", kUiMuted);
}
static void drawSettings(TFT_eSprite& canvas) {
  if (!s_settingsDirty) return;
  s_settingsDirty = false;
  auto& graph = ui::scene::scratchScene();
  buildSettingsScene(graph, canvas.width(), canvas.height());
  graph.render(canvas);
  hal::display::markAllDirty();
}
static void enterSettings() {
  s_settingsDirty = true;
  s_power.leave();
  mp3SetSubState(MP3_SETTINGS);
  hal::display::markAllDirty();
}
const ::scene::Screen kScreenMusicSettings = {
    "Music Settings", enterSettings, nullptr, screenTick, onInput, onChord,
    drawSettings, "settings", buildSettingsScene,
    &screen_chrome_layout::kMusicBrowserTopBarColor,
};
static void showSettings() {
  s_settingsReturn = mp3Sub;
  s_setting = 0;
  s_saveFailed = false;
  ::scene::switchTo(&app, &kScreenMusicSettings);
}
static void closeSettings() {
  services::vibration::navigationFeedback(services::vibration::Cue::Back);
  s_power.activity(millis());
  if (s_settingsReturn == MP3_PLAYING && s_player.isOpen()) showNowPlaying();
  else showBrowser();
}
static void coverMusic() { s_power.leave(); }
static void resumeMusic() { s_power.activity(millis()); }

static const ::scene::Screen* const kScreens[] = {
    &kScreenMusicBrowser,
    &kScreenMusicNowPlaying,
    &kScreenMusicSettings,
};
static AppPresentation presentationPolicy() {
    return {false, false, usesDirectPanel() ? AppPresentation::StatusBar::Hidden : AppPresentation::StatusBar::DirectWhenOptimized};
}
static core::health::RuntimePolicy runtimePolicy() {
    return core::health::whileBusy(isPlaying());
}
App app = [] {
  App result = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Music", "music", &kScreenMusicBrowser,
      mp3Enter, resumeMusic, mp3Exit, "src/apps/mp3_menu_app.cpp", kScreens,
      presentationPolicy, runtimePolicy);
  result.onCover = coverMusic;
  return result;
}();

bool usesDirectPanel() { return mp3UsesDirectPanel(); }

void dumpStatus(Print& out) {
  out.printf("[music-status] active=%u screen=%s files=%d track=%d open=%u playing=%u elapsed_ms=%lu low_power=%u dark=%u radio_off=%u\n",
             s_appActive ? 1u : 0u, mp3Sub == MP3_PLAYING ? "playing" : mp3Sub == MP3_SETTINGS ? "settings" : "browser",
             s_lastFileCount, mp3TrackIdx, s_player.isOpen() ? 1u : 0u,
             s_player.isPlaying() ? 1u : 0u, (unsigned long)s_player.elapsedMs(), s_power.enabled(), s_power.dark(), s_power.radioOff());
}

bool isPlaying() {
  return s_player.isPlaying() && s_player.isOpen();
}

}  // namespace mp3_menu
}  // namespace apps
