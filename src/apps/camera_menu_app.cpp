#include "config/screen_chrome_layout.h"
/**
 * @file camera_menu_app.cpp
 * @brief Camera workflow — live viewfinder and configurable JPEG capture.
 *
 * Sub-states
 * ----------
 *   CAM_INIT        — transient: camera HAL is being initialised
 *   CAM_VIEWFINDER  — live preview; enters optimized-render (direct-TFT) mode
 *   CAM_SETTINGS    — ListView: camera mode, capture resolution, autofocus, manual controls
 *   CAM_ERROR       — camera failure; Back exits, Enter retries
 *
 * Viewfinder pipeline
 * -------------------
 * ui::CameraPreview owns the JPEGDEC workspace, pristine portrait pixel surface
 * and overlay composite. It decodes the centered VGA/QVGA crop, composites the
 * existing photographer UI and shutter pulse, then performs one synchronized
 * native-portrait blit through the existing direct-display HAL path.
 * If surface allocation fails, the legacy QVGA JPEGDecoder MCU path remains:
 * decoded MCU blocks are pushed directly to the panel with a clipped viewport.
 * Camera owns the selected view, stage and animation timing. It supplies input
 * polling as an optional decoder abort callback and retains preview telemetry.
 *
 * Capture flow
 * ------------
 * Triggered by RB in CAM_VIEWFINDER. VGA retains the sensor session; services::camera::Snapshot
 * owns the fresh JPEG copy. The app displays it before saving through
 * services::photos, then uses an interruptible review timer.
 * Higher resolutions and low-memory fallback retain the cold capture flow:
 *   1. camShowShutterFrame() — immediate black shutter feedback.
 *   2. Viewfinder deinit + WiFi powerDown/stop to reclaim contiguous PSRAM.
 *   3. Camera re-init at the target high-res framesize.
 *   4. Camera session writes JPEG to SD; services::photos queues its upload.
 *   5. Camera re-init at preview framesize, viewfinder restarts.
 *
 * PSRAM contiguity
 * ----------------
 * The esp_camera driver allocates the framebuffer as `width × height / 5` bytes of
 * contiguous PSRAM (fixed, not affected by JPEG quality).  For 5 MP (QSXGA) this is
 * ~960 KB. High-res capture retains hal::wifi::powerDown() and its settling
 * delay; lower resolutions use stop(). Both can deinitialize WiFi with the
 * pinned Arduino core, so a distinct RAM saving is not assumed here.
 * services::camera::maxCaptureResolution() gates the target
 * framesize to the largest preset whose buffer will actually fit.
 *
 * ISP tuning
 * ----------
 * The selected mode or persisted manual tuning is applied on the next camera init.
 * SCCB writes after init have been unreliable on this board revision, so changes
 * are committed in the UI and applied when returning to the live camera.
 */

#include "apps/camera_menu_app.h"
#include "ui/safe_area_draw.h"
#include "services/camera_snapshot.h"
#include "services/camera_trial.h"
#include "ui/camera_preview.h"

#include <Arduino.h>
#include "app.h"
#include <esp_camera.h>
#include <esp_timer.h>

#include "hal/hal_display.h"
#include "services/photo_storage.h"
#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "services/camera_settings.h"
#include "core/telemetry.h"
#include "hal/hal_camera.h"
#include "services/camera_session.h"
#include "hal/hal_input.h"
#include "hal/hal_sd.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "ui/draw.h"
#include "ui/home_eyes.h"
#include "ui/list_view.h"
#include "sfx/sfx_lib.h"
#include "ui/nav_controls.h"
#include "ui/scene.h"
#include "ui/source_layout.h"

namespace apps {
namespace camera_menu {

// TFT surface helpers — call DT() in CAM_VIEWFINDER (optimized-render / direct panel),
// T() elsewhere (canvas redirect active; canvas is the render target).
static inline TFT_eSPI& T()  { return hal::display::tft(); }
static inline TFT_eSPI& DT() { return hal::display::directTft(); }

enum CamSubState { CAM_INIT, CAM_VIEWFINDER, CAM_SETTINGS, CAM_MANUAL, CAM_README, CAM_ERROR };

// --- Camera modes ----------------------------------------------------------

using CameraMode = services::camera::Mode;
static constexpr auto CM_MANUAL = CameraMode::Manual;
static constexpr int CM_COUNT = 4;

static const char* kModeLabels[CM_COUNT] = {"Default", "Bright", "Dark", "Manual"};

// --- Capture resolution ----------------------------------------------------

using CapturePreset = services::camera::CaptureResolution;
static constexpr auto CR_VGA = CapturePreset::Vga;
static constexpr auto CR_HD = CapturePreset::Hd;
static constexpr auto CR_5MP = CapturePreset::FiveMp;
static constexpr int CR_COUNT = 3; // Persisted request choices, in service enum order.

// --- State -----------------------------------------------------------------

static CamSubState camState       = CAM_INIT;
static uint32_t    camPhotoCount  = 0;
static char        camStatusMsg[64] = {0};
static uint32_t    s_lastCaptureSeq   = 0;
static char        s_lastCaptureLabel[28] = {0};  // "#N  DD/MM  HH:MM:SS"
static uint32_t    s_captureInfoMs    = 0;        // millis() when last capture finished
static services::camera::Session s_camera;
static uint32_t    s_lastFrameMs  = 0;
static bool        s_continuousVga = false;
static bool        s_reviewActive = false;
static uint32_t    s_reviewStartMs = 0;
static uint32_t    s_reviewDurationMs = 900;
static ui::CameraPreview s_cameraPreview;
static unsigned s_cameraStage = 0;
static uint32_t s_cameraStageAt = 0, s_cameraPulseAt = 0, s_cameraAnimationAt = 0;

static uint32_t    s_vgaSession = 0;
static uint32_t    s_vgaShot = 0;
static uint32_t    s_previewSamples = 0;
static uint64_t    s_previewWindowUs = 0;
static uint32_t    s_previewWindowFrames = 0;
static uint32_t    s_teWaitUs = 0;
static uint32_t    s_teMisses = 0;
static uint32_t    s_previewMaxUs = 0;
static uint32_t    s_decodeUs = 0;
static uint32_t    s_fitUs = 0;
static uint32_t    s_blitUs = 0;
static bool        s_previewInterrupted = false;

extern const ::scene::Screen kScreenCameraViewfinder;
extern const ::scene::Screen kScreenCameraSettings;
extern const ::scene::Screen kScreenCameraManual;
extern const ::scene::Screen kScreenCameraReadme;
extern const ::scene::Screen kScreenCameraError;

static void showViewfinder();
static bool requestViewfinder();
static void showSettings();
static void showManual();
static void showReadme();
static void showError();
static bool camRestorePreviewAfterCapture(uint32_t settleMs);

static int           s_lastModeDrawn   = -1;  // overlay cache; -1 forces first paint
static int           s_settingsSel     = 0;
static bool          s_editMode        = false;
static bool s_editBlink=true;
static uint32_t s_editBlinkMs=0;
static int           s_editBackup      = 0;
static bool          s_settingsNeedsRedraw = false;
static ui::ListView  s_settingsList;
static int           s_manualSel       = 0;
static bool          s_manualNeedsRedraw = false;
static ui::ListView  s_manualList;
static int           s_readmeScroll    = 0;
static bool          s_readmeNeedsRedraw = false;
static services::camera::Settings s_settings;

static const char* kWbModeLabels[5] = {"Auto", "Sunny", "Cloudy", "Office", "Home"};
static const char* kGainLabels[4] = {"2X", "4X", "8X", "16X"};

static int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static int sPreviewFrameSize() { return s_continuousVga ? FRAMESIZE_VGA : FRAMESIZE_QVGA; }

static void camReleaseDecoder() { s_cameraPreview.closeDecoder(); }

static void camReleasePreviewBuffers() {
  camReleaseDecoder();
  s_cameraPreview.close();
  s_cameraStage = 0;
  s_cameraPulseAt = 0;
  s_continuousVga = false;
  s_reviewActive = false;
}

static void camPreparePreviewBuffers() {
  camReleasePreviewBuffers();
  if (!s_cameraPreview.prepareSurfaces()) {
    camReleasePreviewBuffers();
    Serial.println("[camera-vga] no preview scratch memory; using cold capture");
    return;
  }
  s_continuousVga = s_settings.capture == CR_VGA;
}


// Allocate AFTER camera init so its DMA buffers get first claim on internal RAM.
// Fallback preserves camera operation when the remaining heap is fragmented.
static bool camPrepareDecoder() {
  return s_cameraPreview.prepareDecoder();
}

// --- Viewfinder overlay ----------------------------------------------------

static void buildViewfinderScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  char label[28];
  snprintf(label, sizeof(label), "%s", kModeLabels[(int)s_settings.mode]);
  scene.clear();
  ui::scene::setCurrentScene(&scene);

  ui::scene::Node* preview = scene.addRect(
      "camera.preview",
      {0, 0, width, height},
      kUiBg, 0, 0);
  if (preview) preview->visible = false;

  if(s_cameraPreview.hasSurfaces())return; // Raster overlay owns the new full-screen viewfinder.
  const int bandY = height - 28;
  scene.addRect("camera.mode.bg", {0, (int16_t)bandY, width, 28}, kUiBg, 0, 1);
  scene.addLabel("camera.mode.label", {0, (int16_t)(bandY + 2), width, 10},
                 label, kUiFg, kUiBg, 1, ui::scene::TextAlign::TopCenter, 2);
  scene.addLabel("camera.mode.control", {0, (int16_t)(bandY + 14), width, 10},
                 "C: Camera settings", kUiFg, kUiBg, 1, ui::scene::TextAlign::TopCenter, 2);
  ui::scene::applySourceLayout(scene, "camera", "viewfinder");
}

static void camPublishViewfinderScene(bool renderNow) {
  TFT_eSPI& d = DT();
  ui::scene::Scene& scene = ui::scene::scratchScene();
  buildViewfinderScene(scene, d.width(), d.height());

  if (renderNow) {
    hal::display::beginDirectDraw();
    scene.render(d);
    hal::display::endDirectDraw();
  }
}

/**
 * Bottom strip showing the current filter label. Repaints ONLY when the
 * filter index changes, so a steady viewfinder doesn't re-blit the band each
 * frame (that was the visible "flicker" the user reported).
 */
static void camDrawViewfinderOverlay(bool force) {
  if (s_cameraPreview.hasSurfaces()) return;
  const bool showCapture = s_captureInfoMs && (millis() - s_captureInfoMs < 3000u);
  if (!force && !showCapture && s_lastModeDrawn == (int)s_settings.mode) return;

  char modeLabel[28];
  const char* label;
  if (showCapture) {
    label = s_lastCaptureLabel;
  } else {
    snprintf(modeLabel, sizeof(modeLabel), "%s", kModeLabels[(int)s_settings.mode]);
    label = modeLabel;
  }

  TFT_eSPI& d = DT();
  const int W = d.width();
  const int H = d.height();
  const int bandY = H - 28;

  hal::display::beginDirectDraw();
  d.fillRect(0, bandY, W, 28, kUiBg);
  d.setTextDatum(TC_DATUM);
  d.setTextFont(1);
  d.setTextSize(1);
  d.setTextColor(kUiFg, kUiBg);
  d.setTextSize(1);
  ui::safe_area::drawString(d, label, W / 2, bandY + 4);
  ui::safe_area::drawString(d, "C: Camera settings", W / 2, bandY + 14);
  d.setTextDatum(TL_DATUM);
  hal::display::endDirectDraw();

  camPublishViewfinderScene(false);
  if (!showCapture) s_lastModeDrawn = (int)s_settings.mode;
}

static void camPresentPhotographer() {
 if(!s_cameraPreview.hasFrame())return;
 const uint32_t now=millis();
 const unsigned age=std::min(999u,(unsigned)(now-s_cameraStageAt));
 const int pulseAge=s_cameraPulseAt && now-s_cameraPulseAt<140u ? int(now-s_cameraPulseAt) : -1;
 const auto presented=s_cameraPreview.present(s_cameraStage*1000+age,pulseAge);
 if(presented.syncMissed)++s_teMisses;
 s_teWaitUs=presented.syncWaitUs;
 s_cameraAnimationAt=now;
}
static void camPhotographerStage(unsigned stage){
 s_cameraStage=stage;s_cameraStageAt=millis();camPresentPhotographer();
}

// --- Frame render ----------------------------------------------------------

static bool camPreviewShouldAbort(void* context) {
  auto& lastInputMs = *static_cast<uint32_t*>(context);
  const uint32_t now = millis();
  if ((uint32_t)(now - lastInputMs) < 8u) return false;
  lastInputMs = now;
  // Sample/enqueue only; never dispatch navigation while decoding a held frame.
  hal::input::service(now);
  return hal::input::buttonADown() || hal::input::buttonBDown() || hal::input::buttonCDown();
}

static bool camDrawVgaJpeg(const uint8_t* data, size_t len, bool interruptible = false) {
  s_previewInterrupted = false;
  if (!s_cameraPreview.ready() || len > INT32_MAX) return false;
  const uint64_t startUs = esp_timer_get_time();
  uint32_t lastInputMs = millis();
  const auto result = s_cameraPreview.decode(data, len,
      interruptible ? camPreviewShouldAbort : nullptr, &lastInputMs);
  s_previewInterrupted = result == ui::CameraPreview::Result::Interrupted;
  if (result != ui::CameraPreview::Result::Decoded) return false;
  const uint64_t decodedUs = esp_timer_get_time();
  camPresentPhotographer();
  s_decodeUs = (uint32_t)(decodedUs - startUs); // Includes fused fitting callback.
  s_fitUs = 0;
  s_blitUs = (uint32_t)(esp_timer_get_time() - decodedUs);
  return true;
}

static bool camRenderJpeg(const hal::camera::CameraFrame& fb) {
  if (fb.format != PIXFORMAT_JPEG) return false;
  if (s_cameraPreview.ready()) {
    const uint64_t startUs = esp_timer_get_time();
    if (!s_previewWindowUs) s_previewWindowUs = startUs;
    s_previewInterrupted = false;
    if (fb.buf && fb.len &&
        camDrawVgaJpeg(fb.buf, fb.len, true)) {
      const uint32_t elapsedUs = (uint32_t)(esp_timer_get_time() - startUs);
      if (elapsedUs > s_previewMaxUs) s_previewMaxUs = elapsedUs;
      ++s_previewSamples;
      if (++s_previewWindowFrames == 60) {
        const uint64_t nowUs = esp_timer_get_time();
        const double fps = 60000000.0 / (double)(nowUs - s_previewWindowUs);
        Serial.printf("[camera-vga] preview samples=%lu max_us=%lu decode_us=%lu fit_us=%lu blit_us=%lu fps=%.2f te_wait_us=%lu te_misses=%lu\n",
                      (unsigned long)s_previewSamples, (unsigned long)s_previewMaxUs,
                      (unsigned long)s_decodeUs, (unsigned long)s_fitUs, (unsigned long)s_blitUs,
                      fps, (unsigned long)s_teWaitUs, (unsigned long)s_teMisses);
        s_previewWindowUs = nowUs;
        s_previewWindowFrames = 0;
      }
      camDrawViewfinderOverlay(false);
      return true;
    }
    // An intentionally aborted preview is not a sensor/decode failure.
    return s_previewInterrupted;
  }
  if (!s_cameraPreview.presentFallback(fb.buf, fb.len)) return false;
  camDrawViewfinderOverlay(false);
  return true;
}

// --- Capture ---------------------------------------------------------------

static void camDrawReviewStatus(const char* status);

static void camShowShutterFrame() {
  // Keep the last image on screen while acquiring/saving the photo.
  camDrawReviewStatus("Capturing...");
}

static void camDrawReviewStatus(const char* status) {
  if(s_cameraPreview.hasFrame()){
   unsigned stage=status&&strncmp(status,"Saved",5)==0?5:
     status&&strncmp(status,"Saving",6)==0?3:
     status&&strncmp(status,"Capturing",9)==0?1:6;
   camPhotographerStage(stage);
   // Raise immediately while the synchronous acquisition owns the UI task.
   if(stage==1){s_cameraStageAt=millis()-35;camPresentPhotographer();}
   return;
  }
  TFT_eSPI& d = DT();
  const int W = d.width();
  const int H = d.height();
  const int footerH = 20;

  hal::display::beginDirectDraw();
  d.fillRect(0, H - footerH, W, footerH, kUiBg);
  d.setTextDatum(TC_DATUM);
  d.setTextFont(1);
  d.setTextSize(1);
  d.setTextColor(kUiFg, kUiBg);
  ui::safe_area::drawString(d, status ? status : "Saving...", W / 2, H - footerH + 6);
  d.setTextDatum(TL_DATUM);
  hal::display::endDirectDraw();
  hal::display::markAllDirty();

  ui::scene::Scene& scene = ui::scene::scratchScene();
  scene.clear();
  ui::scene::setCurrentScene(&scene);
  scene.addRect("camera.review.bg", {0, 0, (int16_t)W, (int16_t)H}, kUiBg, 0, 0);
  scene.addLabel("camera.review.status", {0, (int16_t)(H - footerH), (int16_t)W, (int16_t)footerH},
                 status ? status : "Saving...", kUiFg, kUiBg, 1,
                 ui::scene::TextAlign::MiddleCenter, 1);

}

static bool camShowCapturedPreview(const char* path, const char* status) {
  if (!path) return false;
  hal::display::beginDirectDraw();
  DT().fillScreen(kUiBg);
  hal::display::endDirectDraw();
  const bool drawn = hal::display::drawJpegFileScaled(path, 0, 0, DT().width(), DT().height());
  camDrawReviewStatus(status);
  return drawn;
}

static bool camReportPreparation(services::camera::Session::PrepareResult result) {
  if (result == services::camera::Session::PrepareResult::CameraBusy) {
    LOGW("camera", "prepare blocked: camera session still owned");
    snprintf(camStatusMsg, sizeof(camStatusMsg), "Camera busy");
  } else if (result == services::camera::Session::PrepareResult::BackgroundBusy) {
    LOGW("camera", "prepare blocked: background network did not stop");
    snprintf(camStatusMsg, sizeof(camStatusMsg), "Upload in progress");
  } else if (result == services::camera::Session::PrepareResult::AgentBusy) {
    LOGW("camera", "prepare blocked: agent transport still active");
    snprintf(camStatusMsg, sizeof(camStatusMsg), "Agent network busy");
  }
  return result == services::camera::Session::PrepareResult::Ready;
}

static bool camStopWifiForCamera(bool deepPowerDown) {
  return camReportPreparation(s_camera.prepareCapture(deepPowerDown));
}

static bool camPrepareViewfinderConsumer() {
  if (!camPrepareDecoder()) return false;
  hal::display::enterOptimizedRender();
  return true;
}

static bool camStartViewfinder() {
  LOGI("camera", "viewfinder requested");
  camStatusMsg[0] = 0;
  if (!camStopWifiForCamera(false)) return false;
  camPreparePreviewBuffers();
  const hal::camera::Tuning tuning = s_settings.tuning();
  if (!s_camera.openPreview(sPreviewFrameSize(), tuning, camPrepareViewfinderConsumer)) {
    camReleasePreviewBuffers();
    snprintf(camStatusMsg, sizeof(camStatusMsg), "Camera init failed");
    LOGE("camera", "viewfinder init failed");
    return false;
  }
  // ST7789 panel needs INVON for normal RGB orientation; main UI flow already toggles this from
  // various places (Kami, eyes), so re-assert here so the very first camera frame matches the
  // rest of the shell.
  hal::display::fixColorInversion();
  hal::display::beginDirectDraw();
  DT().fillScreen(kUiBg);
  hal::display::endDirectDraw();
  /* Status strip is owned by SystemStatusBar in main_menu; leave y in [0, kSystemStatusBarH) untouched. */
  hal::input::configureRotaryEncoderPins();
  s_lastModeDrawn = -1;  // force one overlay paint on entry
  s_lastFrameMs = millis();
  Serial.println("[cam] Viewfinder started");
  s_previewSamples = 0;
  s_previewWindowUs = 0;
  s_previewWindowFrames = 0;
  s_teMisses = 0;
  s_previewMaxUs = 0;
  if (s_continuousVga) {
    ++s_vgaSession;
    s_vgaShot = 0;
    Serial.printf("[camera-vga] session=%lu ready\n", (unsigned long)s_vgaSession);
  }
  return true;
}

static void camStopViewfinder() {
  hal::display::exitOptimizedRender();
  s_camera.stop();
  camReleasePreviewBuffers();
  delay(100);
  Serial.println("[cam] Camera stopped");
}

static bool camRestorePreviewAfterCapture(uint32_t settleMs) {
  s_cameraStage=0;s_cameraPulseAt=0;
  s_reviewActive = false;
  camReleaseDecoder();
  const hal::camera::Tuning tuning = s_settings.tuning();
  if (!s_camera.restartPreview(sPreviewFrameSize(), tuning, settleMs, camPrepareDecoder)) {
    camReleasePreviewBuffers();
    LOGE("camera", "preview restore failed after capture");
    snprintf(camStatusMsg, sizeof(camStatusMsg), "Camera restart failed");
    showError();
    return false;
  }
  hal::display::beginDirectDraw();
  DT().fillScreen(kUiBg);
  hal::display::endDirectDraw();
  s_lastModeDrawn = -1;
  s_lastFrameMs = millis();
  return true;
}

static void camRecordCapture(uint32_t seq) {
  camPhotoCount++;
  s_lastCaptureSeq = seq;
  struct tm tinfo;
  if (getLocalTime(&tinfo, 0)) {
    snprintf(s_lastCaptureLabel, sizeof(s_lastCaptureLabel),
             "#%lu  %02d/%02d  %02d:%02d:%02d", (unsigned long)seq,
             tinfo.tm_mday, tinfo.tm_mon + 1, tinfo.tm_hour, tinfo.tm_min, tinfo.tm_sec);
  } else {
    snprintf(s_lastCaptureLabel, sizeof(s_lastCaptureLabel), "#%lu", (unsigned long)seq);
  }
  s_captureInfoMs = millis();
}

static void camBeginReview(const char* status, uint32_t durationMs = 900) {
  camDrawReviewStatus(status);
  s_reviewStartMs = millis();
  s_reviewDurationMs = durationMs;
  s_reviewActive = true;
}

static bool camCaptureContinuousVga() {
  const uint64_t requestUs = esp_timer_get_time();
  s_reviewActive = false;
  Serial.printf("[camera-vga] BEGIN session=%lu shot=%lu request_us=%llu\n",
                (unsigned long)s_vgaSession, (unsigned long)++s_vgaShot,
                (unsigned long long)requestUs);
  camShowShutterFrame();
  if (!services::photos::available()) {
    camBeginReview("SD unavailable", 1800);
    Serial.println("[camera-vga] ERR sd_unavailable");
    return false;
  }

  services::camera::Snapshot snapshot;
  const auto result = snapshot.captureVga(s_camera, requestUs);
  if (result == services::camera::Snapshot::Result::FrameUnavailable) {
    Serial.println("[camera-vga] ERR fresh_frame_unavailable; recovering");
    if (camRestorePreviewAfterCapture(400)) camBeginReview("Capture failed", 1800);
    return false;
  }
  if (result == services::camera::Snapshot::Result::OutOfMemory) {
    camBeginReview("Not enough memory", 1800);
    Serial.println("[camera-vga] ERR snapshot_allocation");
    return false;
  }
  const auto timing = snapshot.timing();
  const size_t bytes = snapshot.size();

  s_cameraStage=2;s_cameraStageAt=millis();s_cameraPulseAt=millis();
  camPresentPhotographer(); // Actual fresh buffer copied: one whole-screen pulse.
  const bool rendered = camDrawVgaJpeg(snapshot.data(), bytes);
  camDrawReviewStatus(rendered ? "Saving..." : "Saving (no preview)");
  const uint64_t reviewUs = esp_timer_get_time();
  const auto photo = services::photos::prepare();
  const bool pathOk = photo.valid();
  const uint64_t writeStartUs = esp_timer_get_time();
  const bool saved = services::photos::write(photo, snapshot.data(), bytes);
  const uint64_t writtenUs = esp_timer_get_time();
  snapshot.clear();
  bool queued = false;
  if (saved) {
    queued = services::photos::enqueue(photo);
    camRecordCapture(photo.sequence);
  }
  camBeginReview(saved ? (queued ? "Saved" : "Saved (not queued)") : "Save failed",
                 saved ? 900 : 1800);
  Serial.printf("[camera-vga] END ok=%d path=%s ready_ms=%lu copy_ms=%lu review_ms=%lu "
                "write_ms=%lu total_ms=%lu stale=%u dma_us=%llu bytes=%u render=%d queued=%d "
                "decode_us=%lu fit_us=%lu blit_us=%lu\n",
                saved ? 1 : 0, pathOk ? photo.path : "-",
                (unsigned long)((timing.readyUs - requestUs) / 1000),
                (unsigned long)((timing.copiedUs - timing.readyUs) / 1000),
                (unsigned long)((reviewUs - requestUs) / 1000),
                (unsigned long)((writtenUs - writeStartUs) / 1000),
                (unsigned long)((esp_timer_get_time() - requestUs) / 1000), timing.stale,
                (unsigned long long)timing.dmaUs, (unsigned)bytes, rendered ? 1 : 0, queued ? 1 : 0,
                (unsigned long)s_decodeUs, (unsigned long)s_fitUs, (unsigned long)s_blitUs);
  return saved;
}

static bool camCaptureHighRes() {
  camShowShutterFrame();

  const auto photo = services::photos::prepare();
  if (!photo.valid()) {
    hal::display::clearContentArea(kUiBg);
    return false;
  }

  const auto prepared = s_camera.prepareStill(s_settings.capture, s_settings.tuning());
  using StillResult = services::camera::Session::StillResult;
  if (prepared != StillResult::Ready) {
    if (prepared == StillResult::CameraBusy)
      camReportPreparation(services::camera::Session::PrepareResult::CameraBusy);
    else if (prepared == StillResult::BackgroundBusy)
      camReportPreparation(services::camera::Session::PrepareResult::BackgroundBusy);
    else if (prepared == StillResult::AgentBusy)
      camReportPreparation(services::camera::Session::PrepareResult::AgentBusy);
    else
      snprintf(camStatusMsg, sizeof(camStatusMsg), "Capture failed");
    (void)camRestorePreviewAfterCapture(400);
    return false;
  }

  const bool saved = s_camera.captureToFile(photo.path);
  if (saved) {
    const uint32_t reviewStartMs = millis();
    (void)camShowCapturedPreview(photo.path, "Saving...");
    if (!services::photos::enqueue(photo)) {
      Serial.printf("[cam] Saved %s but upload queue full — photo kept on SD without upload retry\n", photo.path);
    }
    camRecordCapture(photo.sequence);
    const uint32_t reviewElapsedMs = millis() - reviewStartMs;
    if (reviewElapsedMs < 900u) delay(900u - reviewElapsedMs);
  }

  return camRestorePreviewAfterCapture(500) && saved;
}

// --- Capture diagnostics ---------------------------------------------------
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool runSettleTrial(uint32_t runId, int discardFrames, unsigned repeat) {
  if (NavHandler::instance().top() != &app || camState != CAM_VIEWFINDER) {
    Serial.println("[camera-settle] ERR args_or_camera_not_ready");
    return false;
  }
  services::camera::SettleTrial trial;
  if (!trial.prepare(s_camera, runId, discardFrames, repeat)) return false;
  const hal::camera::Tuning tuning = s_settings.tuning();
  trial.begin(tuning, kModeLabels[(int)s_settings.mode]);
  camShowShutterFrame();
  const bool saved = trial.capture(s_camera, tuning,
      [](services::camera::Session::PrepareResult result) { (void)camReportPreparation(result); });
  if (saved) {
    char label[24];
    snprintf(label, sizeof(label), "%s R%u", trial.settingLabel(), repeat);
    (void)camShowCapturedPreview(trial.photoPath(), label);
    delay(900);
  }
  const bool restored = camRestorePreviewAfterCapture(500);
  return trial.finish(restored);
}
#endif

// --- Settings UI -----------------------------------------------------------
//
// Row layout (driven by enum below). Order is shown top-down; ISP rows live next to Capture so the
// user lands on color tuning the same trip they pick a resolution.
enum SettingsRow {
  SR_MODE = 0,
  SR_AUTOFOCUS,
  SR_CAPTURE,
  SR_MANUAL,
  SR_COUNT
};

static int settingsRowCount() {
  return s_settings.mode == CM_MANUAL ? SR_COUNT : SR_COUNT - 1;
}

static void camSettingsLabel(int i, char* buf, size_t cap) {
    switch (i) {
        case SR_MODE:
            snprintf(buf, cap, "Camera mode  %s", kModeLabels[(int)s_settings.mode]);
            break;
        case SR_AUTOFOCUS:
            snprintf(buf, cap, "Autofocus  %s", s_settings.autofocus ? "ON" : "OFF");
            break;
        case SR_CAPTURE: {
            const CapturePreset maxAllowed = services::camera::maxCaptureResolution();
            if ((int)s_settings.capture > (int)maxAllowed)
                snprintf(buf, cap, "Capture  %s (!mem)", services::camera::captureLabel(s_settings.capture));
            else
                snprintf(buf, cap, "Capture  %s", services::camera::captureLabel(s_settings.capture));
            break;
        }
        case SR_MANUAL:
            snprintf(buf, cap, "Manual settings");
            break;
        default: buf[0] = 0; break;
    }
}

static ui::ListItem camSettingsItemFn(int idx, void*) {
    static char label[56];
    camSettingsLabel(idx, label, sizeof(label));
    ui::ListItem item;
    item.editing=s_editMode && idx==s_settingsSel; item.blinkOn=s_editBlink;
    item.opensScreen = idx == SR_MANUAL;
    item.label = label;
    item.fg = kUiFg;
    return item;
}

static void buildSettingsScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
    scene.clear();
    ui::scene::setCurrentScene(&scene);
    ui::scene::Builder b(scene, width, height);
    b.clearContent(kUiBg);
    s_settingsList.totalItems = settingsRowCount();
    s_settingsList.selected   = s_settingsSel;
    s_settingsList.buildScene(scene, 0, kSystemStatusBarH, width,
                              height - kSystemStatusBarH,
                              camSettingsItemFn, nullptr,
                              kUiSelBg,
                              kUiSelFg, kUiBg, kUiMuted, kUiFg);
    ui::scene::applySourceLayout(scene, "camera", "settings");
}

static void camDrawSettings(TFT_eSprite& canvas) {
    if (!s_settingsNeedsRedraw) return;
    s_settingsNeedsRedraw = false;
    ui::scene::Scene& scene = ui::scene::scratchScene();
    buildSettingsScene(scene, canvas.width(), canvas.height());
    scene.render(canvas);
    hal::display::markAllDirty();
}

static void prepareSettingsMode() {
  if (camState == CAM_VIEWFINDER || s_camera.running()) camStopViewfinder();
  s_camera.restoreUi(services::camera::Session::Resume::Immediate);
  camState = CAM_SETTINGS;
  s_editMode = false;
  s_settingsSel = clampInt(s_settingsSel, 0, settingsRowCount() - 1);
  s_settingsNeedsRedraw = true;
}

// --- Manual settings UI ----------------------------------------------------

enum ManualRow {
  MR_EXPOSURE = 0,
  MR_GAIN,
  MR_WB,
  MR_SATURATION,
  MR_CONTRAST,
  MR_SHARPNESS,
  MR_README,
  MR_COUNT
};

static void manualLabel(int i, char* buf, size_t cap) {
  switch (i) {
    case MR_EXPOSURE:
      snprintf(buf, cap, "Exposure  %+d", (int)s_settings.manual.ae_level);
      break;
    case MR_GAIN: {
      const uint8_t g = s_settings.manual.gainceiling_idx > 3 ? 3 : s_settings.manual.gainceiling_idx;
      snprintf(buf, cap, "Gain  %s", kGainLabels[g]);
      break;
    }
    case MR_WB: {
      const uint8_t m = s_settings.manual.wb_mode > 4 ? 4 : s_settings.manual.wb_mode;
      snprintf(buf, cap, "White balance  %s", kWbModeLabels[m]);
      break;
    }
    case MR_SATURATION:
      snprintf(buf, cap, "Saturation  %+d", (int)s_settings.manual.saturation);
      break;
    case MR_CONTRAST:
      snprintf(buf, cap, "Contrast  %+d", (int)s_settings.manual.contrast);
      break;
    case MR_SHARPNESS:
      snprintf(buf, cap, "Sharpness  %+d", (int)s_settings.manual.sharpness);
      break;
    case MR_README:
      snprintf(buf, cap, "README");
      break;
    default:
      buf[0] = 0;
      break;
  }
}

static ui::ListItem manualItemFn(int idx, void*) {
  static char label[64];
  manualLabel(idx, label, sizeof(label));
  ui::ListItem item;
  item.opensScreen = idx == MR_README;
  item.editing = s_editMode && idx == s_manualSel;
  item.blinkOn = s_editBlink;
  item.label = label;
  item.fg = kUiFg;
  return item;
}

static void buildManualScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  ui::scene::setCurrentScene(&scene);
  ui::scene::Builder b(scene, width, height);
  b.clearContent(kUiBg);
  s_manualList.totalItems = MR_COUNT;
  s_manualList.selected = s_manualSel;
  s_manualList.buildScene(scene, 0, kSystemStatusBarH, width,
                          height - kSystemStatusBarH,
                          manualItemFn, nullptr,
                          kUiSelBg,
                          kUiSelFg, kUiBg, kUiMuted, kUiFg);
  ui::scene::applySourceLayout(scene, "camera", "manual");
}

static void camDrawManual(TFT_eSprite& canvas) {
  if (!s_manualNeedsRedraw) return;
  s_manualNeedsRedraw = false;
  ui::scene::Scene& scene = ui::scene::scratchScene();
  buildManualScene(scene, canvas.width(), canvas.height());
  scene.render(canvas);
  hal::display::markAllDirty();
}

static void prepareManualMode() {
  if (camState == CAM_VIEWFINDER || s_camera.running()) {
    camStopViewfinder();
    s_camera.restoreUi(services::camera::Session::Resume::Immediate);
  }
  camState = CAM_MANUAL;
  s_editMode = false;
  s_manualSel = clampInt(s_manualSel, 0, MR_COUNT - 1);
  s_manualNeedsRedraw = true;
}

static const char* const kReadmeLines[] = {
    "Manual Mode Help",
    "",
    "Exposure",
    "Sets the auto-exposure target.",
    "Turn up when the image is too dark, faces are underexposed, or indoor scenes look muddy.",
    "Turn down when highlights blow out, windows turn white, or faces look washed out.",
    "",
    "Gain",
    "Sets maximum sensor gain.",
    "Turn up in dark rooms when exposure alone cannot brighten the image enough.",
    "Turn down in bright scenes or when shadows look noisy or speckled.",
    "",
    "White balance",
    "Chooses the color preset.",
    "Turn toward Sunny, Cloudy, Office, or Home when Auto gives the scene the wrong tint.",
    "Turn back toward Auto when lighting changes often or presets look too warm or cool.",
    "",
    "Saturation",
    "Controls color intensity.",
    "Turn up when colors look flat or washed out.",
    "Turn down when colors look fake, skin is too red, or strong colors clip.",
    "",
    "Contrast",
    "Controls dark-to-bright separation.",
    "Turn up when the image looks hazy or flat.",
    "Turn down when shadows are crushed, highlights clip, or faces look harsh.",
    "",
    "Sharpness",
    "Adds edge enhancement.",
    "Turn up when details look soft after focus is correct.",
    "Turn down when edges have halos, text shimmers, or noise gets crunchy.",
};

static constexpr int kReadmeLineCount = sizeof(kReadmeLines) / sizeof(kReadmeLines[0]);

static void buildReadmeScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  ui::scene::setCurrentScene(&scene);
  ui::scene::Builder b(scene, width, height);
  b.clearContent(kUiBg);

  const int areaX = 12;
  const int areaY = kSystemStatusBarH + 6;
  const int areaW = width - 24;
  const int lineH = 16;
  const int visible = (height - areaY - 10) / lineH;
  const int maxScroll = kReadmeLineCount > visible ? kReadmeLineCount - visible : 0;
  s_readmeScroll = clampInt(s_readmeScroll, 0, maxScroll);

  for (int slot = 0; slot < visible; ++slot) {
    const int idx = s_readmeScroll + slot;
    if (idx >= kReadmeLineCount) break;
    char id[32];
    snprintf(id, sizeof(id), "readme.line.%d", slot);
    scene.addLabel(id, {(int16_t)areaX, (int16_t)(areaY + slot * lineH), (int16_t)(areaW - 12), 16},
                   kReadmeLines[idx], kUiFg, kUiBg, 1, ui::scene::TextAlign::TopLeft, 2);
  }

  if (maxScroll > 0) {
    const int sbX = width - 10;
    const int sbY = areaY;
    const int sbH = visible * lineH;
    int thumbH = (sbH * visible) / kReadmeLineCount;
    if (thumbH < 12) thumbH = 12;
    const int thumbY = sbY + ((sbH - thumbH) * s_readmeScroll) / maxScroll;
    scene.addRect("readme.scroll.track", {(int16_t)sbX, (int16_t)sbY, 4, (int16_t)sbH}, kUiMuted, 2, 1);
    scene.addRect("readme.scroll.thumb", {(int16_t)sbX, (int16_t)thumbY, 4, (int16_t)thumbH}, kUiSelBg, 2, 2);
  }
  ui::scene::applySourceLayout(scene, "camera", "readme");
}

static void camDrawReadme(TFT_eSprite& canvas) {
  if (!s_readmeNeedsRedraw) return;
  s_readmeNeedsRedraw = false;
  ui::scene::Scene& scene = ui::scene::scratchScene();
  buildReadmeScene(scene, canvas.width(), canvas.height());
  scene.render(canvas);
  hal::display::markAllDirty();
}

static void prepareReadmeMode() {
  camState = CAM_README;
  s_readmeNeedsRedraw = true;
}

static void persistTopLevelSetting(int row) {
  using Field = services::camera::Setting;
  static constexpr Field fields[] = {Field::Mode, Field::Autofocus, Field::Capture};
  if (row >= 0 && row < (int)(sizeof(fields) / sizeof(fields[0])))
    s_settings.save(fields[row]);
}

static void persistManualSetting(int row) {
  using Field = services::camera::Setting;
  static constexpr Field fields[] = {Field::Exposure, Field::Gain, Field::WhiteBalance, Field::Saturation, Field::Contrast, Field::Sharpness};
  if (row >= 0 && row < (int)(sizeof(fields) / sizeof(fields[0])))
    s_settings.save(fields[row]);
}

static void buildErrorScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  ui::scene::setCurrentScene(&scene);
  ui::scene::Builder b(scene, width, height);
  b.clearContent(kUiBg);
  b.statusLine("camera.error", camStatusMsg, kUiFg);
  b.statusLine("camera.error.hint", "Press: Retry   LB: Exit", kUiMuted);
  ui::scene::applySourceLayout(scene, "camera", "error");
}

static void camDrawError() {
  ui::scene::Scene& scene = ui::scene::scratchScene();
  buildErrorScene(scene, T().width(), T().height());
  scene.render(T());
  hal::display::markAllDirty();
}

// --- Lifecycle -------------------------------------------------------------

static void camEnter() {
  camState = CAM_INIT;
  Serial.println("[cam] Entering Camera");
  // DO NOT endSession() here. Camera doesn't render eyes, so freeing the ~222 KB of PSRAM eye
  // sprites only saves memory we don't need to save (PSRAM is 8 MB; camera framebuffers come from
  // a separate DMA-capable pool). The cost was real: re-running beginSession() after camera exit
  // re-reads the BMP frames from SD over a shared SPI bus, ~6 s of pure blocking that showed up
  // in traces as a single 6 s loop iteration right after `servicePendingRestore` and made every
  // post-camera return feel "stuck". Keeping sprites resident makes the return instant.
  camPhotoCount  = 0;
  camStatusMsg[0] = 0;
  s_settings.load();
  s_lastModeDrawn   = -1;
  s_settingsSel     = 0;
  s_manualSel       = 0;
  s_readmeScroll    = 0;
  s_editMode        = false;
  (void)services::photos::initialize();

  ui::showAppOpeningSplash("Camera");
  // camStartViewfinder() takes ~1 s and doesn't touch the canvas, so the splash
  // stays visible until the viewfinder takes over via enterOptimizedRender().
  (void)requestViewfinder();
}

static void camExit() {
  Serial.println("[cam] Exiting Camera");
  if (camState == CAM_VIEWFINDER || s_camera.running()) camStopViewfinder();
  camReleasePreviewBuffers();
  s_camera.restoreUi(services::camera::Session::Resume::DeferredWithInputRecovery);
  camState = CAM_INIT;
}

static void camTick() {
  if(s_editMode && millis()-s_editBlinkMs>=450u) {
    s_editBlinkMs=millis(); s_editBlink=!s_editBlink;
    s_settingsNeedsRedraw=s_manualNeedsRedraw=true;
  }
  if (camState == CAM_VIEWFINDER) {
    if (s_reviewActive) {
      if ((uint32_t)(millis() - s_reviewStartMs) < s_reviewDurationMs) {
        if(s_cameraPreview.hasSurfaces() && millis()-s_cameraAnimationAt>=40u &&
           (millis()-s_cameraStageAt<220u || (s_cameraPulseAt&&millis()-s_cameraPulseAt<180u)))
          camPresentPhotographer();
        return;
      }
      s_cameraStage=0;s_cameraStageAt=millis();s_cameraPulseAt=0;
      s_reviewActive = false;
      s_previewWindowUs = 0;
      s_previewWindowFrames = 0;
      // The first complete live frame replaces the review without a blank flash.
      s_lastModeDrawn = -1;
      s_lastFrameMs = millis();
    }
    hal::camera::CameraFrame fb;
    if (s_camera.read(&fb)) {
      if (camRenderJpeg(fb)) s_lastFrameMs = millis();
      s_camera.returnFrame();
    }
    if (s_lastFrameMs && (uint32_t)(millis() - s_lastFrameMs) > 2500u) {
      LOGW("camera", "viewfinder frame timeout");
      snprintf(camStatusMsg, sizeof(camStatusMsg), "Camera frame timeout");
      showError();
    }
  }
}

// --- Input -----------------------------------------------------------------

void handleRotary(int steps) {
  if (steps == 0) return;
  sfx::play(sfx::kSfxAlarmNavigate);
  if (camState == CAM_VIEWFINDER) {
    return;
  }
  if (camState == CAM_SETTINGS && !s_editMode) {
    const int prev = s_settingsSel;
    s_settingsSel = clampInt(s_settingsSel + steps, 0, settingsRowCount() - 1);
    if (prev != s_settingsSel) {
      s_settingsList.selected = s_settingsSel;
      s_settingsNeedsRedraw = true;
      hal::display::markAllDirty();
    }
    return;
  }
  if (camState == CAM_SETTINGS && s_editMode) {
    switch (s_settingsSel) {
      case SR_MODE: {
        int v = clampInt((int)s_settings.mode + steps, 0, CM_COUNT - 1);
        s_settings.mode = (CameraMode)v;
        s_settingsSel = clampInt(s_settingsSel, 0, settingsRowCount() - 1);
        break;
      }
      case SR_AUTOFOCUS:
        if(abs(steps)%2) s_settings.autofocus=!s_settings.autofocus;
        break;
      case SR_CAPTURE: {
        // Allow the user to scroll to any preset. The "(low mem)" tag in the row label warns when
        // the current selection might not fit, and `camCaptureHighRes()` falls back to HD at
        // shutter time if the chosen preset can't allocate. (Earlier version clamped here, which
        // made the rotary feel dead at low PSRAM — looked like settings were broken.)
        int v = (int)s_settings.capture + steps;
        if (v < 0) v = 0;
        if (v > CR_COUNT - 1) v = CR_COUNT - 1;
        s_settings.capture = (CapturePreset)v;
        break;
      }
      default: break;
    }
    s_settingsNeedsRedraw = true;
    hal::display::markAllDirty();
    return;
  }
  if (camState == CAM_MANUAL && !s_editMode) {
    const int prev = s_manualSel;
    s_manualSel = clampInt(s_manualSel + steps, 0, MR_COUNT - 1);
    if (prev != s_manualSel) {
      s_manualList.selected = s_manualSel;
      s_manualNeedsRedraw = true;
      hal::display::markAllDirty();
    }
    return;
  }
  if (camState == CAM_MANUAL && s_editMode) {
    switch (s_manualSel) {
      case MR_EXPOSURE:
        s_settings.manual.ae_level = (int8_t)clampInt((int)s_settings.manual.ae_level + steps, -2, 2);
        break;
      case MR_GAIN:
        s_settings.manual.gainceiling_idx = (uint8_t)clampInt((int)s_settings.manual.gainceiling_idx + steps, 0, 3);
        break;
      case MR_WB:
        s_settings.manual.wb_mode = (uint8_t)clampInt((int)s_settings.manual.wb_mode + steps, 0, 4);
        break;
      case MR_SATURATION:
        s_settings.manual.saturation = (int8_t)clampInt((int)s_settings.manual.saturation + steps, -2, 2);
        break;
      case MR_CONTRAST:
        s_settings.manual.contrast = (int8_t)clampInt((int)s_settings.manual.contrast + steps, -2, 2);
        break;
      case MR_SHARPNESS:
        s_settings.manual.sharpness = (int8_t)clampInt((int)s_settings.manual.sharpness + steps, -3, 3);
        break;
      default:
        break;
    }
    s_manualNeedsRedraw = true;
    hal::display::markAllDirty();
    return;
  }
  if (camState == CAM_README) {
    s_readmeScroll = clampInt(s_readmeScroll + steps, 0, kReadmeLineCount - 1);
    s_readmeNeedsRedraw = true;
    hal::display::markAllDirty();
  }
}

static void camButtonA() {
  switch (camState) {
    case CAM_VIEWFINDER:
      NavHandler::instance().pop();
      break;
    case CAM_SETTINGS:
      if (s_editMode) {
        switch (s_settingsSel) {
          case SR_MODE:       s_settings.mode      = (CameraMode)s_editBackup; break;
          case SR_AUTOFOCUS:  s_settings.autofocus       = (s_editBackup != 0); break;
          case SR_CAPTURE:    s_settings.capture     = (CapturePreset)s_editBackup; break;
          default: break;
        }
        s_settingsSel = clampInt(s_settingsSel, 0, settingsRowCount() - 1);
        s_editMode = false;
        s_settingsNeedsRedraw = true; hal::display::markAllDirty();
      } else {
        showViewfinder();
      }
      break;
    case CAM_MANUAL:
      if (s_editMode) {
        switch (s_manualSel) {
          case MR_EXPOSURE:   s_settings.manual.ae_level = (int8_t)s_editBackup; break;
          case MR_GAIN:       s_settings.manual.gainceiling_idx = (uint8_t)s_editBackup; break;
          case MR_WB:         s_settings.manual.wb_mode = (uint8_t)s_editBackup; break;
          case MR_SATURATION: s_settings.manual.saturation = (int8_t)s_editBackup; break;
          case MR_CONTRAST:   s_settings.manual.contrast = (int8_t)s_editBackup; break;
          case MR_SHARPNESS:  s_settings.manual.sharpness = (int8_t)s_editBackup; break;
          default: break;
        }
        s_editMode = false;
        s_manualNeedsRedraw = true; hal::display::markAllDirty();
      } else {
        showSettings();
      }
      break;
    case CAM_README:
      showManual();
      break;
    case CAM_ERROR:
      NavHandler::instance().pop();
      break;
    default:
      break;
  }
}

static void camButtonB() {
  switch (camState) {
    case CAM_VIEWFINDER: {
      if (s_continuousVga && s_settings.capture == CR_VGA && s_camera.running()) {
        (void)camCaptureContinuousVga();
      } else {
        (void)camCaptureHighRes();
      }
      break;
    }
    case CAM_SETTINGS:
      if (!s_editMode) {
        if (s_settingsSel == SR_MANUAL) {
          showManual();
          return;
        }
        s_editMode = true;
        switch (s_settingsSel) {
          case SR_MODE:       s_editBackup = (int)s_settings.mode; break;
          case SR_AUTOFOCUS:  s_editBackup = s_settings.autofocus ? 1 : 0; break;
          case SR_CAPTURE:    s_editBackup = (int)s_settings.capture; break;
          default:            s_editBackup = 0; break;
        }
        s_settingsNeedsRedraw = true; hal::display::markAllDirty();
      } else {
        persistTopLevelSetting(s_settingsSel);
        s_editMode = false;
        s_settingsNeedsRedraw = true; hal::display::markAllDirty();
        if (s_settingsSel == SR_MODE && s_settings.mode == CM_MANUAL) {
          showManual();
        }
      }
      break;
    case CAM_MANUAL:
      if (!s_editMode) {
        if (s_manualSel == MR_README) {
          showReadme();
          return;
        }
        s_editMode = true;
        switch (s_manualSel) {
          case MR_EXPOSURE:   s_editBackup = (int)s_settings.manual.ae_level; break;
          case MR_GAIN:       s_editBackup = (int)s_settings.manual.gainceiling_idx; break;
          case MR_WB:         s_editBackup = (int)s_settings.manual.wb_mode; break;
          case MR_SATURATION: s_editBackup = (int)s_settings.manual.saturation; break;
          case MR_CONTRAST:   s_editBackup = (int)s_settings.manual.contrast; break;
          case MR_SHARPNESS:  s_editBackup = (int)s_settings.manual.sharpness; break;
          default:            s_editBackup = 0; break;
        }
        s_manualNeedsRedraw = true; hal::display::markAllDirty();
      } else {
        persistManualSetting(s_manualSel);
        s_editMode = false;
        s_manualNeedsRedraw = true; hal::display::markAllDirty();
      }
      break;
    case CAM_ERROR:
      showViewfinder();
      break;
    default:
      break;
  }
}

void enter()   { camEnter(); }
void exitApp() { camExit(); }
void tick()    { camTick(); }

void onDraw(TFT_eSprite& canvas) {
  if (camState == CAM_SETTINGS) camDrawSettings(canvas);
  else if (camState == CAM_MANUAL) camDrawManual(canvas);
  else if (camState == CAM_README) camDrawReadme(canvas);
}

void onInput(hal::input::InputEvent ev) {
  using ET = hal::input::EventType;
  if (ui::nav::isBackTap(ev)) {
    camButtonA();
  } else if (camState == CAM_VIEWFINDER && ev.type == ET::ButtonB_Tap) {
    // Commit capture on release, only after HAL rules out an A+B chord.
    // A press can arrive before the other finger during a back gesture.
    camButtonB();
  } else if (ui::nav::isEnterTap(ev)) {
    if (camState == CAM_VIEWFINDER) showSettings();
    else camButtonB();
  } else if (ev.type == ET::Scroll && ev.delta != 0) {
    handleRotary(ev.delta);
  }
}

// App-owned mode transitions request operations; screen entry only refreshes views.
static bool requestViewfinder() {
  const bool returningFromSettings = camState == CAM_SETTINGS ||
      camState == CAM_MANUAL || camState == CAM_README;
  if (returningFromSettings) camState = CAM_VIEWFINDER;
  if (camStartViewfinder()) {
    camState = CAM_VIEWFINDER;
    return true;
  }
  if (returningFromSettings || !camStatusMsg[0])
    snprintf(camStatusMsg, sizeof(camStatusMsg), "Camera init failed");
  showError();
  return false;
}

static void resumeCameraWorkflow() {
  if (app.currentScreen == &kScreenCameraViewfinder) (void)requestViewfinder();
  else if (app.currentScreen == &kScreenCameraSettings) prepareSettingsMode();
  else if (app.currentScreen == &kScreenCameraManual) prepareManualMode();
  else if (app.currentScreen == &kScreenCameraReadme) prepareReadmeMode();
}

static void enterViewfinderScreen() {
  camPublishViewfinderScene(false);
}

static void enterSettingsScreen() {
  s_settingsNeedsRedraw = true;
}

static void enterManualScene() {
  s_manualNeedsRedraw = true;
}

static void enterReadmeScene() {
  s_readmeNeedsRedraw = true;
}

static void enterErrorScreen() {
  camDrawError();
}

static void viewfinderTick(uint32_t) {
  camTick();
}

const ::scene::Screen kScreenCameraViewfinder = {
    "Viewfinder",
    enterViewfinderScreen,
    nullptr,
    viewfinderTick,
    onInput,
    nullptr,
    nullptr,
    "viewfinder",
    buildViewfinderScene,
    &kUiBg,
};

const ::scene::Screen kScreenCameraSettings = {
    "Settings",
    enterSettingsScreen,
    nullptr,
    viewfinderTick,
    onInput,
    nullptr,
    onDraw,
    "settings",
    buildSettingsScene,
    &screen_chrome_layout::kCameraSettingsTopBarColor,
};

const ::scene::Screen kScreenCameraManual = {
    "Manual",
    enterManualScene,
    nullptr,
    viewfinderTick,
    onInput,
    nullptr,
    onDraw,
    "manual",
    buildManualScene,
};

const ::scene::Screen kScreenCameraReadme = {
    "README",
    enterReadmeScene,
    nullptr,
    nullptr,
    onInput,
    nullptr,
    onDraw,
    "readme",
    buildReadmeScene,
};

const ::scene::Screen kScreenCameraError = {
    "Error",
    enterErrorScreen,
    nullptr,
    nullptr,
    onInput,
    nullptr,
    nullptr,
    "error",
    buildErrorScene,
    &screen_chrome_layout::kCameraErrorTopBarColor,
};

static void showViewfinder() {
  if (requestViewfinder()) ::scene::switchTo(&app, &kScreenCameraViewfinder);
}

static void showSettings() {
  prepareSettingsMode();
  ::scene::switchTo(&app, &kScreenCameraSettings);
}

static void showManual() {
  prepareManualMode();
  ::scene::switchTo(&app, &kScreenCameraManual);
}

static void showReadme() {
  prepareReadmeMode();
  ::scene::switchTo(&app, &kScreenCameraReadme);
}

static void showError() {
  camState = CAM_ERROR;
  // Release the camera before WiFi can reclaim DMA/internal RAM. Otherwise
  // an error both retains camera buffers and leaves Fleet polling suspended.
  camStopViewfinder();
  if (!s_camera.running()) {
    s_camera.restoreUi(services::camera::Session::Resume::Deferred);
  } else {
    LOGW("camera", "error cleanup blocked; network remains paused until camera releases");
  }
  if (app.currentScreen == &kScreenCameraError) {
    enterErrorScreen();
    return;
  }
  ::scene::switchTo(&app, &kScreenCameraError);
}

static const ::scene::Screen* const kScreens[] = {
    &kScreenCameraViewfinder,
    &kScreenCameraSettings,
    &kScreenCameraManual,
    &kScreenCameraReadme,
    &kScreenCameraError,
};
App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Camera", "camera", &kScreenCameraViewfinder, camEnter, resumeCameraWorkflow, camExit,
                            "src/apps/camera_menu_app.cpp", kScreens, nullptr, nullptr);

}  // namespace camera_menu
}  // namespace apps
