#include "config/screen_chrome_layout.h"
/**
 * @file audio_recordings_app.cpp
 * @brief Audio Recordings app: browse and play WAV clips from /recordings.
 */

#include "apps/audio_recordings_app.h"
#include "ui/safe_area_draw.h"

#include <Arduino.h>
#include <cstring>

#include "app.h"
#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "services/media_delete.h"
#include "services/audio_library.h"
#include "services/wav_player.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "scene/screen.h"
#include "ui/draw.h"
#include "ui/list_view.h"
#include "ui/nav_controls.h"
#include "ui/scene.h"
#include "nav_handler.h"
#include "ui/morning_style.h"
#include "ui/source_layout.h"

namespace apps {
namespace audio_recordings {

static services::audio::Library s_library{services::audio::Library::Kind::Recordings};
static bool s_deleteMarked[services::audio::Library::kCapacity];
static int  s_browseIdx = 0;
static int  s_trackIdx = -1;

static services::audio::WavPlayer s_player;

enum SubState { BROWSER, DELETE_SELECT, CONFIRM_DELETE, PLAYING };
enum DeleteTarget { DELETE_NONE, DELETE_SELECTED };
enum PlayMode { PLAY_LOOP, PLAY_THRU, PLAY_HALT };
static SubState s_sub = BROWSER;
static DeleteTarget s_deleteTarget = DELETE_NONE;
static PlayMode s_playMode = PLAY_HALT;
static bool s_browserNeedsRedraw = false;
static bool s_rbModeHoldFired = false;
static uint32_t s_rbModeHoldStartMs = 0;
static constexpr uint32_t kModeHoldMs = 650;
static constexpr uint32_t kRestartThresholdMs = 1500;

extern const ::scene::Screen kScreenAudioRecordingsBrowser;
extern const ::scene::Screen kScreenAudioRecordingsNowPlaying;

static ui::ListView s_browser;

static constexpr int kVisBars = services::audio::WavPlayer::kVisBars;
static uint8_t s_visHeight[kVisBars];
static uint32_t s_visLastMs = 0;
static uint32_t s_lastProgressMs = 0;

static int gVisX = 8, gVisY = 100, gVisBarW = 10, gVisGap = 2, gVisH = 42;
static int gProgY = 0, gProgW = 0, gProgX = 6;
static int gTransportY = 0;
static constexpr int kTransportBandH = 46;
static constexpr int kVolBarW = 10;

static int sProgLastChunkCount = -1;
static int sProgLastFilledPx = 0;
static uint32_t sProgLastElSec = UINT32_MAX;

static inline bool usesDirect() { return s_sub == PLAYING; }

static inline TFT_eSPI& wavTft() {
  return usesDirect() ? hal::display::directTft() : hal::display::tft();
}

static const char* playModeName() {
  switch (s_playMode) {
    case PLAY_LOOP: return "Loop";
    case PLAY_THRU: return "Thru";
    default: return "Halt";
  }
}

static const char* playModeIcon() {
  switch (s_playMode) {
    case PLAY_LOOP: return "[o]";
    case PLAY_THRU: return "->";
    default: return "-o";
  }
}

static void setSubState(SubState target) {
  if (target == s_sub) return;
  const SubState prev = s_sub;
  s_sub = target;
  if (target == PLAYING && prev != PLAYING) {
    hal::display::enterOptimizedRender();
  } else if (target != PLAYING && prev == PLAYING) {
    hal::display::exitOptimizedRender();
    s_browserNeedsRedraw = true;
  }
}

static inline int volBarX() { return wavTft().width() - kScreenInsetX - kVolBarW; }

static void drawVolumeBarRaw() {
  const int x = volBarX();
  const int y = kSystemStatusBarH + kScreenInsetY;
  const int h = max(44, gVisY - 4 - y);
  ui::Painter s(wavTft());
  s.verticalBar(x, y, kVolBarW, h, s_player.volume(), 100);
}

static void drawVolumeBar() {
  hal::display::beginDirectDraw();
  drawVolumeBarRaw();
  hal::display::endDirectDraw();
}

static void drawVis() {
  uint8_t targets[kVisBars];
  s_player.copySpectrum(targets);
  hal::display::beginDirectDraw();
  ui::Painter s(wavTft());
  s.spectrumBars(gVisX, gVisY, gVisBarW, gVisGap, gVisH, kVisBars,
                 s_visHeight, targets);
  hal::display::endDirectDraw();
}

static void drawProgressBar(bool drawBorder) {
  hal::display::beginDirectDraw();
  ui::Painter s(wavTft());
  sProgLastFilledPx = s.chunkedProgress(gProgX, gProgY, gProgW, 10,
                                        s_player.elapsedMs(),
                                        s_player.trackInfo().durationMs,
                                        sProgLastFilledPx, drawBorder);
  hal::display::endDirectDraw();
}

static void drawProgressTimes() {
  hal::display::beginDirectDraw();
  ui::Painter s(wavTft());
  s.progressTimes(gProgX, gProgY + 12, gProgW,
                  s_player.elapsedMs(), s_player.trackInfo().durationMs);
  hal::display::endDirectDraw();
}

static void drawProgressFull() {
  drawProgressBar(true);
  drawProgressTimes();
}

static void layoutNowPlaying(int width) {
  gVisBarW = 10;
  gVisGap = 2;
  gVisH = 42;

  const int yMeta = kSystemStatusBarH + kScreenInsetY + 20;
  gVisY = yMeta + 14;
  const int visTotalW = kVisBars * (gVisBarW + gVisGap);
  gVisX = kScreenInsetX + max(0, (width - 2 * kScreenInsetX - visTotalW) / 2);
  gTransportY = gVisY + gVisH + 10;
  gProgY = gTransportY + kTransportBandH + 10;
  gProgX = kScreenInsetX;
  gProgW = width - 2 * kScreenInsetX;
}

static void buildNowPlayingScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  ui::scene::setCurrentScene(&scene);
  layoutNowPlaying(width);

  const int textX = kScreenInsetX;
  const int volX = width - kScreenInsetX - kVolBarW;
  const int textW = max(40, volX - textX - 10);
  const auto& info = s_player.trackInfo();

  scene.addRect("recordings.bg", {0, kSystemStatusBarH, width,
                                  (int16_t)(height - kSystemStatusBarH)},
                kUiBg, 0, 0);
  scene.addLabel("recordings.title",
                 {(int16_t)textX, (int16_t)(kSystemStatusBarH + kScreenInsetY),
                  (int16_t)textW, 20},
                 info.title[0] ? info.title : "Recording", kUiFg, kUiBg, 2,
                 ui::scene::TextAlign::TopLeft, 1);
  scene.addLabel("recordings.meta",
                 {(int16_t)textX, (int16_t)(kSystemStatusBarH + kScreenInsetY + 20),
                  (int16_t)textW, 12},
                 "WAV recording", kUiMuted, kUiBg, 1,
                 ui::scene::TextAlign::TopLeft, 1);

  const int volH = max(44, gVisY - 4 - (kSystemStatusBarH + kScreenInsetY));
  scene.addRect("recordings.volume.track",
                {(int16_t)volX, (int16_t)(kSystemStatusBarH + kScreenInsetY),
                 kVolBarW, (int16_t)volH},
                kUiPanel, 3, 1);
  const int volFill = (volH * s_player.volume()) / 100;
  scene.addRect("recordings.volume.fill",
                {(int16_t)volX,
                 (int16_t)(kSystemStatusBarH + kScreenInsetY + volH - volFill),
                 kVolBarW, (int16_t)volFill},
                kUiAccent, 3, 2);

  const uint32_t dur = s_player.trackInfo().durationMs;
  scene.addProgressBar("recordings.progress",
                       {(int16_t)gProgX, (int16_t)gProgY, (int16_t)gProgW, 10},
                       s_player.elapsedMs(), dur ? dur : 1,
                       kUiPanel, kUiAccent, 2, 1);
  scene.addLabel("recordings.transport.prev", {36, (int16_t)(gTransportY + 14), 30, 16},
                 "<<", kUiFg, kUiBg, 2, ui::scene::TextAlign::MiddleCenter, 1);
  scene.addLabel("recordings.transport.prev.label", {18, (int16_t)(gTransportY + 31), 64, 12},
                 "Back", kUiMuted, kUiBg, 1, ui::scene::TextAlign::TopCenter, 1);
  scene.addLabel("recordings.transport.play", {120, (int16_t)(gTransportY + 14), 40, 16},
                 s_player.isPlaying() ? "||" : ">", kUiFg, kUiBg, 2,
                 ui::scene::TextAlign::MiddleCenter, 1);
  scene.addLabel("recordings.transport.mode", {100, (int16_t)(gTransportY + 31), 80, 12},
                 playModeIcon(), kUiAccent, kUiBg, 1, ui::scene::TextAlign::TopCenter, 1);
  scene.addLabel("recordings.transport.next", {214, (int16_t)(gTransportY + 14), 30, 16},
                 ">>", kUiFg, kUiBg, 2, ui::scene::TextAlign::MiddleCenter, 1);
  scene.addLabel("recordings.transport.next.label", {198, (int16_t)(gTransportY + 31), 64, 12},
                 "Next", kUiMuted, kUiBg, 1, ui::scene::TextAlign::TopCenter, 1);
  char modeLabel[32];
  snprintf(modeLabel, sizeof(modeLabel), "Mode: %s", playModeName());
  scene.addLabel("recordings.mode.name", {0, (int16_t)(height - 14), width, 12},
                 modeLabel, kUiMuted, kUiBg, 1, ui::scene::TextAlign::TopCenter, 1);

  const int bandPad = 2;
  scene.addRect("recordings.visualizer.bg",
                {(int16_t)(kScreenInsetX - bandPad), (int16_t)(gVisY - bandPad),
                 (int16_t)(width - 2 * (kScreenInsetX - bandPad)),
                 (int16_t)(gVisH + 2 * bandPad)},
                kUiBg, 0, 0);
  for (int i = 0; i < kVisBars && scene.count() < ui::scene::Scene::kMaxNodes; i++) {
    const int h = max(2, (gVisH * (int)s_visHeight[i]) / 255);
    char id[32];
    snprintf(id, sizeof(id), "recordings.vis.%02d", i);
    scene.addRect(id, {(int16_t)(gVisX + i * (gVisBarW + gVisGap)),
                       (int16_t)(gVisY + gVisH - h),
                       (int16_t)gVisBarW, (int16_t)h},
                  kUiAccent, 2, 1);
  }
  ui::scene::applySourceLayout(scene, "recordings", "now-playing");
}

static void publishNowPlayingScene(bool renderNow) {
  ui::scene::Scene& scene = ui::scene::scratchScene();
  buildNowPlayingScene(scene, wavTft().width(), wavTft().height());
  if (renderNow) {
    hal::display::beginDirectDraw();
    scene.render(wavTft());
    hal::display::endDirectDraw();
  }
}

static void progressSyncCacheFromNow() {
  uint32_t el = s_player.elapsedMs();
  uint32_t dur = s_player.trackInfo().durationMs;
  int filled = (dur > 0 && el < dur)
      ? (int)((int64_t)gProgW * el / dur)
      : (dur > 0 ? gProgW : 0);
  sProgLastChunkCount = filled / 4;
  sProgLastFilledPx = filled;
  sProgLastElSec = el / 1000u;
}

static void redrawProgressIfNeeded() {
  uint32_t el = s_player.elapsedMs();
  uint32_t dur = s_player.trackInfo().durationMs;
  uint32_t elSec = el / 1000u;
  int filled = (dur > 0 && el < dur)
      ? (int)((int64_t)gProgW * el / dur)
      : (dur > 0 ? gProgW : 0);
  int chunkCount = filled / 4;
  bool barChanged = (chunkCount != sProgLastChunkCount);
  bool timeChanged = (elSec != sProgLastElSec);
  if (!barChanged && !timeChanged) return;
  sProgLastChunkCount = chunkCount;
  sProgLastElSec = elSec;
  if (barChanged) drawProgressBar(false);
  if (timeChanged) drawProgressTimes();
}

static void drawTransportRowFull() {
  hal::display::beginDirectDraw();
  TFT_eSPI& t = wavTft();
  const int sw = t.width();
  const int cy = gTransportY + kTransportBandH / 2 - 3;
  const int cx = sw / 2;
  t.fillRect(0, gTransportY, sw, kTransportBandH, kUiBg);
  t.setTextFont(1);
  t.setTextColor(kUiFg, kUiBg);
  t.setTextSize(3);
  t.setTextDatum(MR_DATUM);
  ui::safe_area::drawString(t, "<<", cx - 52, cy);
  t.setTextDatum(ML_DATUM);
  ui::safe_area::drawString(t, ">>", cx + 52, cy);
  t.setTextDatum(MC_DATUM);
  if (s_player.isPlaying()) {
    t.fillRect(cx - 10, cy - 14, 6, 28, kUiFg);
    t.fillRect(cx + 4,  cy - 14, 6, 28, kUiFg);
  } else {
    t.fillTriangle(cx - 9, cy - 16, cx - 9, cy + 16, cx + 18, cy, kUiFg);
  }
  t.setTextSize(1);
  t.setTextColor(kUiMuted, kUiBg);
  ui::safe_area::drawString(t, "Back", cx - 74, gTransportY + kTransportBandH - 7);
  ui::safe_area::drawString(t, "Next", cx + 74, gTransportY + kTransportBandH - 7);
  t.setTextColor(kUiAccent, kUiBg);
  ui::safe_area::drawString(t, playModeIcon(), cx, gTransportY + kTransportBandH - 7);
  hal::display::endDirectDraw();
}

static int browserRowCount() {
  return s_library.count() > 0 ? s_library.count() + 1 : 0;
}

static int deleteRowCount() {
  return s_library.count() > 0 ? s_library.count() + 2 : 0;
}

static int browserSelectedFileIndex() {
  return s_browser.selected > 0 ? s_browser.selected - 1 : -1;
}

static int deleteSelectedFileIndex() {
  return s_browser.selected >= 2 ? s_browser.selected - 2 : -1;
}

static int markedCount() {
  int count = 0;
  for (int i = 0; i < s_library.count(); ++i) {
    if (s_deleteMarked[i]) ++count;
  }
  return count;
}

static bool allMarked() {
  return s_library.count() > 0 && markedCount() == s_library.count();
}

static void clearDeleteMarks() {
  memset(s_deleteMarked, 0, sizeof(s_deleteMarked));
}

static void syncBrowserSelectionToFileIndex() {
  if (s_library.count() <= 0) {
    s_browseIdx = 0;
    s_browser.selected = 0;
    s_browser.scrollFirst = 0;
    return;
  }
  if (s_browseIdx < 0) s_browseIdx = 0;
  if (s_browseIdx >= s_library.count()) s_browseIdx = s_library.count() - 1;
  s_browser.totalItems = browserRowCount();
  s_browser.selected = s_browseIdx + 1;
  s_browser.clampScroll();
}

static ui::ListItem browserItemFn(int idx, void*) {
  static char s_name[48];
  if (s_sub == DELETE_SELECT) {
    if (idx == 0) {
      const int count = markedCount();
      snprintf(s_name, sizeof(s_name), "Delete selected (%d)", count);
    } else if (idx == 1) {
      strlcpy(s_name, allMarked() ? "Clear all" : "Select all", sizeof(s_name));
    } else {
      const int fileIdx = idx - 2;
      char name[40];
      s_library.name(fileIdx, name, sizeof(name));
      snprintf(s_name, sizeof(s_name), "%c %s", s_deleteMarked[fileIdx] ? '*' : ' ', name);
    }
  } else {
    if (idx == 0) {
      strlcpy(s_name, "Delete recordings", sizeof(s_name));
    } else {
      s_library.name(idx - 1, s_name, sizeof(s_name));
    }
  }
  ui::ListItem item;
  item.label = s_name;
  item.opensScreen = s_sub != DELETE_SELECT;
  item.choice = s_sub == DELETE_SELECT && idx >= 2;
  item.checked = item.choice && s_deleteMarked[idx - 2];
  item.fg = (idx == 0) ? TFT_ORANGE : kUiFg;
  return item;
}

static void buildBrowserScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  ui::scene::Builder b(scene, width, height);
  b.clearContent(kUiBg);
  if (s_sub == CONFIRM_DELETE) {
    const int count = markedCount();
    char msg[112];
    snprintf(msg,sizeof(msg),"Delete %d selected recording%s? This cannot be undone.",count,count==1?"":"s");
    ui::kami_message::message(scene,width,height,"recordings.delete",msg);
    ui::morning_style::footer(scene,width,height,"B: Yes",ui::morning_style::Icon::Cancel,"A: Back",ui::morning_style::Icon::Retry);
  } else if (s_sub == DELETE_SELECT) {
    if (s_library.count() == 0) {
      b.emptyState("recordings.empty", "No playable recordings");

    } else {
      s_browser.totalItems = deleteRowCount();
      s_browser.clampScroll();
      s_browser.buildScene(scene, 0, kSystemStatusBarH, width,
                           height - kSystemStatusBarH - 8,
                           browserItemFn, nullptr,
                           kUiSelBg, kUiSelFg, kUiBg, kUiMuted, kUiFg);

    }
  } else if (s_library.count() == 0) {
    b.emptyState("recordings.empty", "No playable recordings");
  } else {
    s_browser.totalItems = browserRowCount();
    s_browser.clampScroll();
    s_browser.buildScene(scene, 0, kSystemStatusBarH, width,
                         height - kSystemStatusBarH - 8,
                         browserItemFn, nullptr,
                         kUiSelBg, kUiSelFg, kUiBg, kUiMuted, kUiFg);

  }
  ui::scene::applySourceLayout(scene, "recordings", "browser");
}

static void drawBrowser(TFT_eSprite& canvas) {
  ui::scene::Scene& scene = ui::scene::scratchScene();
  buildBrowserScene(scene, canvas.width(), canvas.height());
  scene.render(canvas);
  hal::display::markAllDirty();
  s_browserNeedsRedraw = false;
}

static void clearContentArea() {
  wavTft().fillRect(0, kSystemStatusBarH,
                    wavTft().width(), wavTft().height() - kSystemStatusBarH, kUiBg);
}

static void drawNowPlaying() {
  clearContentArea();
  layoutNowPlaying(wavTft().width());
  memset(s_visHeight, 0, sizeof(s_visHeight));
  publishNowPlayingScene(true);
  drawTransportRowFull();
  drawProgressFull();
  progressSyncCacheFromNow();
}

static void showBrowser();
static void showNowPlaying();

static void enterDeleteSelectMode() {
  if (s_library.count() <= 0) return;
  clearDeleteMarks();
  s_browser.totalItems = deleteRowCount();
  s_browser.selected = 0;
  s_browser.scrollFirst = 0;
  setSubState(DELETE_SELECT);
  s_browserNeedsRedraw = true;
  hal::display::markAllDirty();
}

static void requestConfirmSelectedDelete() {
  if (markedCount() <= 0) return;
  s_deleteTarget = DELETE_SELECTED;
  setSubState(CONFIRM_DELETE);
  s_browserNeedsRedraw = true;
  hal::display::markAllDirty();
}

static void cancelDeleteMode() {
  clearDeleteMarks();
  s_deleteTarget = DELETE_NONE;
  setSubState(BROWSER);
  syncBrowserSelectionToFileIndex();
  s_browserNeedsRedraw = true;
  hal::display::markAllDirty();
}

static void cancelDelete() {
  s_deleteTarget = DELETE_NONE;
  setSubState(DELETE_SELECT);
  s_browserNeedsRedraw = true;
  hal::display::markAllDirty();
}

static void deleteMarkedRecordings() {
  const int count = markedCount();
  if (count <= 0) {
    cancelDelete();
    return;
  }

  services::media_delete::Result result;
  for (int i = 0; i < s_library.count(); ++i) {
    if (!s_deleteMarked[i]) continue;
    services::media_delete::deleteFile(s_library[i], &result);
  }
  Serial.printf("[recordings] Delete selected: requested=%d removed=%d missing=%d queue=%d failed=%d\n",
                result.requested, result.removed, result.missing,
                result.queueRemoved, result.failed);
  s_library.load();
  s_browseIdx = 0;
  s_trackIdx = -1;
  clearDeleteMarks();
  s_deleteTarget = DELETE_NONE;
  setSubState(BROWSER);
  syncBrowserSelectionToFileIndex();
  s_browserNeedsRedraw = true;
  hal::display::markAllDirty();
}

static void confirmDelete() {
  deleteMarkedRecordings();
}

static void toggleDeleteSelection() {
  if (s_sub != DELETE_SELECT || s_library.count() <= 0) return;
  if (s_browser.selected == 0) {
    requestConfirmSelectedDelete();
    return;
  }
  if (s_browser.selected == 1) {
    const bool mark = !allMarked();
    for (int i = 0; i < s_library.count(); ++i) s_deleteMarked[i] = mark;
  } else {
    const int fileIdx = deleteSelectedFileIndex();
    if (fileIdx >= 0 && fileIdx < s_library.count()) {
      s_deleteMarked[fileIdx] = !s_deleteMarked[fileIdx];
    }
  }
  s_browserNeedsRedraw = true;
  hal::display::markAllDirty();
}

static bool startTrack(int trackIdx) {
  if (trackIdx < 0 || trackIdx >= s_library.count()) return false;
  s_trackIdx = trackIdx;
  if (!s_player.startTrack(s_library[trackIdx])) {
    s_trackIdx = -1;
    return false;
  }
  s_browseIdx = trackIdx;
  s_browser.selected = trackIdx + 1;
  s_lastProgressMs = 0;
  showNowPlaying();
  return true;
}

static void pausePlayback() {
  s_player.pause();
  s_player.clearSpectrum();
  publishNowPlayingScene(false);
  drawTransportRowFull();
  drawProgressFull();
  progressSyncCacheFromNow();
}

static void resumePlayback() {
  s_player.resume();
  publishNowPlayingScene(false);
  drawTransportRowFull();
  drawProgressFull();
  progressSyncCacheFromNow();
}

static void cyclePlayMode() {
  if (s_playMode == PLAY_LOOP) s_playMode = PLAY_THRU;
  else if (s_playMode == PLAY_THRU) s_playMode = PLAY_HALT;
  else s_playMode = PLAY_LOOP;
  Serial.printf("[recordings] Play mode: %s\n", playModeName());
  publishNowPlayingScene(false);
  drawTransportRowFull();
}

static void restartCurrentTrack() {
  if (!s_player.restartTrack()) return;
  s_lastProgressMs = 0;
  drawNowPlaying();
}

static void playNext() {
  if (s_library.count() <= 0) {
    showBrowser();
    return;
  }
  const int current = s_trackIdx;
  if (!s_player.stopTrack()) return;
  for (int offset = 1; offset <= s_library.count(); ++offset) {
    const int next = (current + offset + s_library.count()) % s_library.count();
    if (startTrack(next)) return;
  }
  Serial.println("[recordings] No playable next track");
  showBrowser();
}

static void handleTrackEnded() {
  if (s_library.count() <= 0) {
    showBrowser();
    return;
  }
  if (s_playMode == PLAY_LOOP) {
    restartCurrentTrack();
  } else if (s_playMode == PLAY_THRU) {
    playNext();
  } else {
    pausePlayback();
  }
}

static void prepareBrowserWorkflow() {
  setSubState(BROWSER);
  s_library.load();
}

static void appResume() {
  if (app.currentScreen == &kScreenAudioRecordingsBrowser) prepareBrowserWorkflow();
}

static void appEnter() {
  ui::showAppOpeningSplash("Audio Recordings");
  s_trackIdx = -1;
  s_sub = BROWSER;
  s_deleteTarget = DELETE_NONE;
  clearDeleteMarks();
  s_browseIdx = 0;
  s_browser.selected = 1;
  s_browser.scrollFirst = 0;
  memset(s_visHeight, 0, sizeof(s_visHeight));
  s_visLastMs = s_lastProgressMs = 0;

  s_player.openSession();
  prepareBrowserWorkflow();
}

static void appExit() {
  s_player.closeSession();
  setSubState(BROWSER);

  s_trackIdx = -1;
  s_library.close();
}

void tick() {
  uint32_t now = millis();
  if (s_sub != PLAYING) return;

  if (s_player.consumeWantsNextTrack()) {
    Serial.printf("[recordings] EOT -> %s\n", playModeName());
    handleTrackEnded();
  }

  if (s_sub == PLAYING && s_rbModeHoldStartMs != 0 && !s_rbModeHoldFired &&
      hal::input::buttonBDown() &&
      (uint32_t)(now - s_rbModeHoldStartMs) >= kModeHoldMs) {
    s_rbModeHoldFired = true;
    cyclePlayMode();
  }

  if (now - s_visLastMs >= 50) {
    if (s_sub == PLAYING) drawVis();
    s_visLastMs = now;
  }

  if (s_sub == PLAYING && s_player.isOpen() && (now - s_lastProgressMs >= 250)) {
    s_lastProgressMs = now;
    redrawProgressIfNeeded();
  }
}

void onInput(hal::input::InputEvent ev) {
  using ET = hal::input::EventType;

  if (ev.type == ET::Scroll && ev.delta != 0) {
    if (s_sub == BROWSER && s_library.count() > 0) {
      s_browser.totalItems = browserRowCount();
      s_browser.scroll(ev.delta);
      const int selectedFile = browserSelectedFileIndex();
      if (selectedFile >= 0) s_browseIdx = selectedFile;
      s_browserNeedsRedraw = true;
      hal::display::markAllDirty();
    } else if (s_sub == DELETE_SELECT && s_library.count() > 0) {
      s_browser.totalItems = deleteRowCount();
      s_browser.scroll(ev.delta);
      s_browserNeedsRedraw = true;
      hal::display::markAllDirty();
    } else if (s_sub == PLAYING) {
      int v = s_player.volume() - ev.delta * 5;
      if (v < 0) v = 0;
      if (v > 100) v = 100;
      s_player.setVolume(v);
      publishNowPlayingScene(false);
      drawVolumeBar();
    }
    return;
  }

  if (ui::nav::isEnterTap(ev) && s_sub == BROWSER) {
    const int selectedFile = browserSelectedFileIndex();
    if (s_library.count() > 0 && selectedFile < 0) enterDeleteSelectMode();
    else if (s_library.count() > 0 && !startTrack(selectedFile)) Serial.println("[recordings] Play failed");
    return;
  }

  if (ev.type == ET::ButtonC_Tap) {
    if (s_sub == PLAYING) {
      if (s_player.isPlaying()) pausePlayback();
      else if (s_player.isOpen()) resumePlayback();
      drawVolumeBar();
    } else if (s_sub == DELETE_SELECT) {
      toggleDeleteSelection();
    }
    return;
  }

  if (ui::nav::isBackTap(ev)) {
    if (s_sub == PLAYING) {
      if (s_player.stopTrack()) showBrowser();
    }
    else if (s_sub == DELETE_SELECT) cancelDeleteMode();
    else if (s_sub == CONFIRM_DELETE) cancelDelete();
    else NavHandler::instance().pop();
    return;
  }

  if (ev.type == ET::ButtonB_Press && s_sub == PLAYING) {
    s_rbModeHoldStartMs = millis();
    s_rbModeHoldFired = false;
    return;
  }

  if (ev.type == ET::ButtonB_Release && s_sub == PLAYING) {
    s_rbModeHoldStartMs = 0;
    return;
  }

  if (ev.type == ET::ButtonB_Tap) {
    if (s_sub == PLAYING) {
      s_rbModeHoldStartMs = 0;
      if (s_rbModeHoldFired) {
        s_rbModeHoldFired = false;
        return;
      }
      playNext();
    }
    else if (s_sub == CONFIRM_DELETE) confirmDelete();
    else if (s_sub == DELETE_SELECT) requestConfirmSelectedDelete();
    else if (s_sub == BROWSER) {
      const int selectedFile = browserSelectedFileIndex();
      if (selectedFile < 0) enterDeleteSelectMode();
    }
    return;
  }
}

void onDraw(TFT_eSprite& canvas) {
  if (s_sub == PLAYING || !s_browserNeedsRedraw) return;
  drawBrowser(canvas);
}

void enter() { appEnter(); }
void exitApp() { appExit(); }

bool onChord() {
  if (s_sub == PLAYING) {
    if (s_player.stopTrack()) showBrowser();
    return true;
  }
  if (s_sub == CONFIRM_DELETE) {
    cancelDelete();
    return true;
  }
  if (s_sub == DELETE_SELECT) {
    cancelDeleteMode();
    return true;
  }
  return false;
}

static void enterBrowserScreen() {
  syncBrowserSelectionToFileIndex();
  s_browserNeedsRedraw = true;
  hal::display::markAllDirty();
}

static void enterNowPlayingScreen() {
  setSubState(PLAYING);
  drawNowPlaying();
}

static void screenTick(uint32_t) {
  tick();
}

const ::scene::Screen kScreenAudioRecordingsBrowser = {
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

const ::scene::Screen kScreenAudioRecordingsNowPlaying = {
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
  prepareBrowserWorkflow();
  if (app.currentScreen == &kScreenAudioRecordingsBrowser) {
    enterBrowserScreen();
    return;
  }
  ::scene::switchTo(&app, &kScreenAudioRecordingsBrowser, ::scene::ScreenTransition::Back);
}

static void showNowPlaying() {
  if (app.currentScreen == &kScreenAudioRecordingsNowPlaying) {
    enterNowPlayingScreen();
    return;
  }
  ::scene::switchTo(&app, &kScreenAudioRecordingsNowPlaying);
}

static const ::scene::Screen* const kScreens[] = {
    &kScreenAudioRecordingsBrowser,
    &kScreenAudioRecordingsNowPlaying,
};

static AppPresentation presentationPolicy() {
    return {false, false, AppPresentation::StatusBar::DirectWhenOptimized};
}
static core::health::RuntimePolicy runtimePolicy() {
    return core::health::whileBusy(isPlaying());
}
App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Audio Recordings", "recordings",
                            &kScreenAudioRecordingsBrowser, appEnter, appResume, appExit,
                            "src/apps/audio_recordings_app.cpp", kScreens, presentationPolicy, runtimePolicy);

bool usesDirectPanel() { return usesDirect(); }

bool isPlaying() {
  return s_player.isPlaying() && s_player.isOpen() && s_sub == PLAYING;
}

}  // namespace audio_recordings
}  // namespace apps
