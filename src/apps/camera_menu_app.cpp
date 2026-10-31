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
#if defined(ORIGAMI_ASYNC_CAMERA_PREVIEW)
#include "services/camera_preview_decode.h"
#endif
#include "apps/tutorial_app.h"
#include "ui/safe_area_draw.h"
#include "services/camera_snapshot.h"
#include "services/camera_trial.h"
#include "ui/camera_preview.h"
#include "ui/onboarding_approved_view.h"

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
#include "core/usage_events.h"
#include "hal/hal_camera.h"
#include "services/camera_session.h"
#include "services/video_recording.h"
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
static services::camera::VideoRecording s_video;
#if defined(ORIGAMI_ASYNC_CAMERA_PREVIEW)
static services::camera::PreviewDecode s_previewDecode;
static uint64_t s_previewSubmittedUs=0;
#endif
static constexpr int kDefaultShootMode = 0;
// Video has a sustained SD/decoder bandwidth budget. QR and still photos retain
// their existing HAL quality defaults; this value applies only to video mode.
static constexpr int kVideoJpegQuality = 12;
static constexpr int kHdVideoJpegQuality = 16;
static int s_shootMode = kDefaultShootMode;
static const char* const kShootLabels[] = {"Photo", "Video silent", "Video + mic"};
static constexpr int kShootModes = sizeof(kShootLabels) / sizeof(kShootLabels[0]);
static bool s_videoPending = false, s_videoStopping = false;
static uint32_t s_videoStartedMs = 0, s_videoPreviewMs = 0;
static uint32_t s_videoNoticeMs = 0;
static char s_videoNotice[32]{};
// UI-task shutter gesture. HAL still owns debouncing and A+B chord detection.
static constexpr uint32_t kVideoHoldMs = 1200;
static bool s_shutterArmed = false, s_shutterSkipTap = false, s_quickVideo = false;
static uint32_t s_shutterPressedMs = 0;
static void camFinishQuickVideo() {
  if (s_quickVideo) s_shootMode = kDefaultShootMode;
  s_quickVideo = false;
  s_shutterArmed = false;
}

// UI-owned pipeline measurements; sensor timestamps distinguish acquisition
// cadence from the recording submission gate and display work.
static struct VideoPerf {
  uint32_t reads=0, unique=0, previews=0, maxReadUs=0, maxPreviewUs=0, maxBytes=0;
  uint64_t readUs=0, previewUs=0, firstSensorUs=0, lastSensorUs=0;
} s_videoPerf;

static bool camVideoHd();

static const char* camVideoLabel() {
  using State = services::camera::VideoRecording::State;
  static char label[32];
  const auto state = s_video.state();
  if (state == State::Starting) return "STARTING VIDEO";
  if (s_videoStopping || state == State::Finishing) return "SAVING VIDEO";
  if (state == State::Recording) {
    const uint32_t seconds = (millis() - s_videoStartedMs) / 1000;
    snprintf(label, sizeof(label), "REC %02lu:%02lu  B: STOP",
             (unsigned long)(seconds / 60), (unsigned long)(seconds % 60));
    return label;
  }
  if (s_videoNoticeMs && millis() - s_videoNoticeMs < 4000) return s_videoNotice;
  if (!s_shootMode) return nullptr;
  snprintf(label,sizeof(label),"%s %s  B: REC",camVideoHd()?"720p":"VGA",s_shootMode==2?"+ MIC":"SILENT");
  return label;
}
static uint32_t    s_lastFrameMs  = 0;
static bool s_tutorialRequested=false,s_tutorialCapture=false;
static services::photos::Target s_tutorialPhoto;
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
#ifdef ORIGAMI_DEV_SERIAL_INPUT
// One bounded presentation window; no per-frame serial traffic. DMA timestamps
// measure driver-to-display age, not optical exposure or glass-to-glass latency.
static struct {
  uint64_t decodeUs=0, blitUs=0, ageUs=0, gapUs=0, lastPresentedUs=0, lastDmaUs=0;
  uint32_t ageSamples=0, repeatedDma=0, maxAgeUs=0, maxGapUs=0;
} s_previewTiming;
#endif
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
// Video supports VGA and HD; higher still-photo presets use the explicitly
// displayed HD video setting without changing the saved photo preference.
static bool camVideoHd() { return s_settings.capture != CR_VGA; }
static int camVideoQuality() {
  return !s_shootMode ? -1 : s_continuousVga && camVideoHd() ? kHdVideoJpegQuality : kVideoJpegQuality;
}

static const char* kWbModeLabels[5] = {"Auto", "Sunny", "Cloudy", "Office", "Home"};
static const char* kGainLabels[4] = {"2X", "4X", "8X", "16X"};

static int clampInt(int v, int lo, int hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static int sPreviewFrameSize() {
  if (s_shootMode && s_continuousVga && camVideoHd()) return FRAMESIZE_HD;
  return s_continuousVga ? FRAMESIZE_VGA : FRAMESIZE_QVGA;
}

static int sPreviewPixelFormat() {
#if defined(ORIGAMI_RGB_PREVIEW_TRIAL)
  if (s_shootMode && !s_continuousVga && s_cameraPreview.hasSurfaces()) return PIXFORMAT_RGB565;
#endif
  return PIXFORMAT_JPEG;
}

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
  // Video framing only needs the display-sized stream. Recording explicitly
  // switches to VGA before starting its writer/microphone session.
  s_continuousVga = !s_shootMode && s_settings.capture == CR_VGA;
}


// Allocate the synchronous codec after camera DMA setup. The experimental
// worker may already own its codec; do not allocate a second one on restart.
// Fallback preserves camera operation when the remaining heap is fragmented.
static bool camPrepareDecoder() {
  if (sPreviewPixelFormat()==PIXFORMAT_RGB565) {s_cameraPreview.closeDecoder();return true;}
#if (defined(ORIGAMI_VIDEO_HD_TRIAL) && !defined(ORIGAMI_VIDEO_HD_PREVIEW_TRIAL)) || defined(ORIGAMI_VIDEO_CAPTURE_ONLY_TRIAL)
  if (s_shootMode && s_continuousVga) return true; // Capture-only benchmark.
#endif
#if defined(ORIGAMI_ASYNC_CAMERA_PREVIEW)
  if (s_previewDecode.active()) return true;
#endif
  if (s_shootMode && s_continuousVga && camVideoHd()) return s_cameraPreview.prepareDecoder(true,false,true);
  return s_cameraPreview.prepareDecoder(false, s_continuousVga);
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
  const char* videoLabel = camVideoLabel();
  if (!force && !videoLabel && !showCapture && s_lastModeDrawn == (int)s_settings.mode) return;

  char modeLabel[28];
  const char* label;
  if (videoLabel) {
    label = videoLabel;
  } else if (showCapture) {
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
 const auto presented=s_cameraPreview.present(s_cameraStage*1000+age,pulseAge,
     s_tutorialCapture ? nullptr : (!s_shootMode && !s_cameraStage ? (s_shutterArmed && now-s_shutterPressedMs>=300u ? "HOLD B FOR VIDEO" : "B: PHOTO / HOLD: VIDEO") : camVideoLabel()), s_video.busy(), s_tutorialCapture);
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
  return hal::input::buttonADown() || (hal::input::buttonBDown() && !s_shutterArmed && !s_shutterSkipTap) || hal::input::buttonCDown();
}

static bool camDrawPreview(const uint8_t* data, size_t len, bool interruptible = false, bool fit = false, bool rawRgb565 = false) {
  s_previewInterrupted = false;
  if (!(rawRgb565 ? s_cameraPreview.hasSurfaces() : s_cameraPreview.ready()) || len > INT32_MAX) return false;
  const uint64_t startUs = esp_timer_get_time();
  uint32_t lastInputMs = millis();
  const auto result = rawRgb565
      ? s_cameraPreview.copyRgb565(data,len,interruptible ? camPreviewShouldAbort : nullptr,&lastInputMs)
      : s_cameraPreview.decode(data,len,interruptible ? camPreviewShouldAbort : nullptr,&lastInputMs,fit);
  s_previewInterrupted = result == ui::CameraPreview::Result::Interrupted;
  if (result != ui::CameraPreview::Result::Decoded) return false;
  const uint64_t decodedUs = esp_timer_get_time();
  camPresentPhotographer();
  s_decodeUs = (uint32_t)(decodedUs - startUs); // Includes fused fitting callback.
  s_fitUs = 0;
  s_blitUs = (uint32_t)(esp_timer_get_time() - decodedUs);
  return true;
}

static bool camRenderFrame(const hal::camera::CameraFrame& fb) {
  const bool raw=fb.format==PIXFORMAT_RGB565 && fb.width==320 && fb.height==240;
  if (fb.format!=PIXFORMAT_JPEG && !raw) return false;
  if (raw && !s_cameraPreview.hasSurfaces()) return false;
  if (raw || s_cameraPreview.ready()) {
    const uint64_t startUs = esp_timer_get_time();
    if (!s_previewWindowUs) {
      s_previewWindowUs = startUs;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
      s_previewTiming = {};
#endif
    }
    s_previewInterrupted = false;
    if (fb.buf && fb.len &&
        camDrawPreview(fb.buf, fb.len, true, fb.width == 1280 && fb.height == 720, raw)) {
      const uint32_t elapsedUs = (uint32_t)(esp_timer_get_time() - startUs);
      if (elapsedUs > s_previewMaxUs) s_previewMaxUs = elapsedUs;
      ++s_previewSamples;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
      const uint64_t presentedUs=esp_timer_get_time();
      auto& timing=s_previewTiming;
      timing.decodeUs+=s_decodeUs;timing.blitUs+=s_blitUs;
      if (timing.lastPresentedUs) {
        const uint64_t gap=presentedUs-timing.lastPresentedUs;
        timing.gapUs+=gap;
        timing.maxGapUs=std::max(timing.maxGapUs,uint32_t(std::min(gap,uint64_t(UINT32_MAX))));
      }
      timing.lastPresentedUs=presentedUs;
      if (fb.dmaStartUs && fb.dmaStartUs<=presentedUs) {
        const uint64_t age=presentedUs-fb.dmaStartUs;
        timing.ageUs+=age;++timing.ageSamples;
        timing.maxAgeUs=std::max(timing.maxAgeUs,uint32_t(std::min(age,uint64_t(UINT32_MAX))));
        if (fb.dmaStartUs==timing.lastDmaUs) ++timing.repeatedDma;
        timing.lastDmaUs=fb.dmaStartUs;
      }
#endif
      if (++s_previewWindowFrames == 60) {
        const uint64_t nowUs = esp_timer_get_time();
        const double fps = 60000000.0 / (double)(nowUs - s_previewWindowUs);
        Serial.printf("[camera-vga] preview samples=%lu max_us=%lu decode_us=%lu fit_us=%lu blit_us=%lu fps=%.2f te_wait_us=%lu te_misses=%lu\n",
                      (unsigned long)s_previewSamples, (unsigned long)s_previewMaxUs,
                      (unsigned long)s_decodeUs, (unsigned long)s_fitUs, (unsigned long)s_blitUs,
                      fps, (unsigned long)s_teWaitUs, (unsigned long)s_teMisses);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
        Serial.printf("[camera-preview-timing] mode=%s frames=60 decode_mean_us=%llu blit_mean_us=%llu gap_mean_us=%llu gap_max_us=%lu dma_age_samples=%lu dma_age_mean_us=%llu dma_age_max_us=%lu repeated_dma=%lu\n",
            s_videoPending ? "recording" : "idle",
            (unsigned long long)(timing.decodeUs/60), (unsigned long long)(timing.blitUs/60),
            (unsigned long long)(timing.gapUs/59), (unsigned long)timing.maxGapUs,
            (unsigned long)timing.ageSamples,
            (unsigned long long)(timing.ageSamples ? timing.ageUs/timing.ageSamples : 0),
            (unsigned long)timing.maxAgeUs, (unsigned long)timing.repeatedDma);
        timing = {};
#endif
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
  if(s_tutorialCapture&&!s_cameraPreview.hasSurfaces()){
    snprintf(camStatusMsg,sizeof(camStatusMsg),"Camera preview memory unavailable");return false;
  }
  if (!camPrepareDecoder()) return false;
  hal::display::enterOptimizedRender();
  return true;
}

static hal::camera::Tuning camPreviewTuning() {
  auto tuning=s_settings.tuning();
  tuning.allowNightMode=s_shootMode==0;
  return tuning;
}

static bool camStartViewfinder() {
  LOGI("camera", "viewfinder requested");
  camStatusMsg[0] = 0;
  if (!camStopWifiForCamera(false)) return false;
  camPreparePreviewBuffers();
  const hal::camera::Tuning tuning = camPreviewTuning();
  if (!s_camera.openPreview(sPreviewFrameSize(), tuning, camPrepareViewfinderConsumer, camVideoQuality(), sPreviewPixelFormat())) {
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

// Report terminal recordings from both normal Stop and workflow teardown.
// Caller has observed completion or close() has acknowledged all workers.
static void camReportVideoResult() {
  const bool saved = s_video.state() == services::camera::VideoRecording::State::Saved;
  Serial.printf("[video] %s path=%s frames=%lu dropped=%lu\n", saved ? "saved" : "failed",
      s_video.path(), (unsigned long)s_video.frames(), (unsigned long)s_video.dropped());
  if (!saved) Serial.printf("[video-error] %s\n", s_video.error());
  const auto submitted=s_video.submissionStats();
  const auto writer=s_video.writerStats();
  Serial.printf("[video-write] jpeg_chunks=%lu jpeg_bytes=%lu repeated_chunks=%lu repeated_bytes=%lu pcm_bytes=%lu chunk_ms=%lu max_chunk_ms=%lu\n",
      (unsigned long)writer.jpegChunks,(unsigned long)writer.jpegBytes,
      (unsigned long)writer.repeatedJpegChunks,(unsigned long)writer.repeatedJpegBytes,
      (unsigned long)writer.pcmBytes,(unsigned long)writer.chunkWriteMs,(unsigned long)writer.maxChunkWriteMs);
  Serial.printf("[video-submit] accepted=%lu queue_full=%lu oversized=%lu invalid=%lu max_queued=%lu\n",
      (unsigned long)submitted.accepted, (unsigned long)submitted.queueFull,
      (unsigned long)submitted.oversized, (unsigned long)submitted.invalid,
      (unsigned long)submitted.maxQueued);
  Serial.printf("[video-audio] samples=%lu repeated=%lu\n",
      (unsigned long)s_video.audioSamples(), (unsigned long)s_video.repeatedFrames());
  const auto captured=s_video.captureStats();
  s_videoPerf.reads=captured.reads;s_videoPerf.unique=captured.unique;
  s_videoPerf.readUs=captured.readUs;s_videoPerf.maxReadUs=captured.maxReadUs;
  s_videoPerf.maxBytes=captured.maxBytes;s_videoPerf.firstSensorUs=captured.firstSensorUs;
  s_videoPerf.lastSensorUs=captured.lastSensorUs;
  Serial.printf("[video-perf] reads=%lu unique=%lu sensor_span_us=%llu read_us=%llu read_max_us=%lu previews=%lu preview_us=%llu preview_max_us=%lu jpeg_max_bytes=%lu\n",
      (unsigned long)s_videoPerf.reads, (unsigned long)s_videoPerf.unique,
      (unsigned long long)(s_videoPerf.lastSensorUs-s_videoPerf.firstSensorUs),
      (unsigned long long)s_videoPerf.readUs, (unsigned long)s_videoPerf.maxReadUs,
      (unsigned long)s_videoPerf.previews, (unsigned long long)s_videoPerf.previewUs,
      (unsigned long)s_videoPerf.maxPreviewUs, (unsigned long)s_videoPerf.maxBytes);
}

static void camStopViewfinder() {
#if defined(ORIGAMI_ASYNC_CAMERA_PREVIEW)
  s_previewDecode.close();
#endif
  // Retain camera/background admission until SD has drained and closed.
  s_video.close();
  camFinishQuickVideo();
  s_shutterSkipTap = false;
  if (s_videoPending) camReportVideoResult();
  s_videoPending = s_videoStopping = false;
  NavHandler::instance().runtimePolicyChanged();
  hal::display::exitOptimizedRender();
  // The direct camera surface replaced the panel, including the clock strip.
  // Settings must repaint cached chrome even when its values/color are unchanged.
  NavHandler::instance().invalidateChrome();
  s_camera.stop();
  camReleasePreviewBuffers();
  delay(100);
  Serial.println("[cam] Camera stopped");
}

static bool camRestorePreviewAfterCapture(uint32_t settleMs) {
  s_cameraStage=0;s_cameraPulseAt=0;
  s_reviewActive = false;
  camReleaseDecoder();
  const hal::camera::Tuning tuning = camPreviewTuning();
  if (!s_camera.restartPreview(sPreviewFrameSize(), tuning, settleMs, camPrepareDecoder, camVideoQuality(), sPreviewPixelFormat())) {
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
  // Do not mix recording/settle time into the next live-preview FPS window.
  s_previewWindowUs = 0;
  s_previewWindowFrames = 0;
  s_previewMaxUs = 0;
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
    core::usage::emit("camera","camera.failed");
    return false;
  }

  services::camera::Snapshot snapshot;
  const auto result = snapshot.captureVga(s_camera, requestUs);
  if (result == services::camera::Snapshot::Result::FrameUnavailable) {
    Serial.println("[camera-vga] ERR fresh_frame_unavailable; recovering");
    if (camRestorePreviewAfterCapture(400)) camBeginReview("Capture failed", 1800);
    core::usage::emit("camera","camera.failed");
    return false;
  }
  if (result == services::camera::Snapshot::Result::OutOfMemory) {
    camBeginReview("Not enough memory", 1800);
    Serial.println("[camera-vga] ERR snapshot_allocation");
    core::usage::emit("camera","camera.failed");
    return false;
  }
  const auto timing = snapshot.timing();
  const size_t bytes = snapshot.size();

  s_cameraStage=2;s_cameraStageAt=millis();s_cameraPulseAt=millis();
  camPresentPhotographer(); // Actual fresh buffer copied: one whole-screen pulse.
  const bool rendered = camDrawPreview(snapshot.data(), bytes);
  camDrawReviewStatus(rendered ? "Saving..." : "Saving (no preview)");
  const uint64_t reviewUs = esp_timer_get_time();
  const auto photo = services::photos::prepare();
  const bool pathOk = photo.valid();
  const uint64_t writeStartUs = esp_timer_get_time();
  const bool saved = services::photos::write(photo, snapshot.data(), bytes);
  if (!saved) core::usage::emit("camera","camera.failed");
  const uint64_t writtenUs = esp_timer_get_time();
  snapshot.clear();
  bool queued = false;
  if (saved) {
    queued = services::photos::enqueue(photo);
    core::usage::emit("camera","camera.saved");
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
    core::usage::emit("camera","camera.failed");
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
    core::usage::emit("camera","camera.failed");
    return false;
  }

  const bool saved = s_camera.captureToFile(photo.path);
  if (!saved) core::usage::emit("camera","camera.failed");
  if (saved) {
    const uint32_t reviewStartMs = millis();
    (void)camShowCapturedPreview(photo.path, "Saving...");
    if (!services::photos::enqueue(photo)) {
      Serial.printf("[cam] Saved %s but upload queue full — photo kept on SD without upload retry\n", photo.path);
    }
    core::usage::emit("camera","camera.saved");
    camRecordCapture(photo.sequence);
    const uint32_t reviewElapsedMs = millis() - reviewStartMs;
    if (reviewElapsedMs < 900u) delay(900u - reviewElapsedMs);
  }

  return camRestorePreviewAfterCapture(500) && saved;
}

// --- Capture diagnostics ---------------------------------------------------
static bool camBeginVideo() {
  if (s_video.busy() || s_videoPending) return false;
#if defined(ORIGAMI_ASYNC_CAMERA_PREVIEW)
  s_previewSubmittedUs=0;
  if (s_cameraPreview.hasSurfaces()) {
    s_cameraPreview.closeDecoder();
    const bool decoding=s_previewDecode.begin();
    if (!decoding) s_cameraPreview.prepareDecoder(false,s_continuousVga);
    Serial.printf("[camera-preview-worker] started=%u\n", decoding?1u:0u);
  }
#endif
  // Reconfigure within the existing camera reservation, before the microphone
  // starts. Never label QVGA payload as the selected recording size if restart fails.
  if (!s_continuousVga) {
    s_continuousVga = true;
    if (!camRestorePreviewAfterCapture(700)) {
#if defined(ORIGAMI_ASYNC_CAMERA_PREVIEW)
      s_previewDecode.close();
#endif
      return false;
    }
  }
  s_videoPerf = {};
  s_reviewActive = false;
  s_cameraStage = 0;
  s_videoStartedMs = millis();
  s_videoPreviewMs = 0;
  s_videoStopping = false;
  s_videoPending = true;
  NavHandler::instance().runtimePolicyChanged();
  const uint16_t width = camVideoHd() ? 1280 : 640;
  const uint16_t height = camVideoHd() ? 720 : 480;
#if defined(ORIGAMI_VIDEO_CAPTURE_ONLY_TRIAL)
  camDrawReviewStatus("Recording - B to stop");
  bool started=s_video.beginCamera(s_camera, width, height, s_shootMode == 2, false);
#else
  bool started=s_video.beginCamera(s_camera, width, height, s_shootMode == 2);
#endif
#if defined(ORIGAMI_ASYNC_CAMERA_PREVIEW)
  if (!started && s_previewDecode.active()) {
    // Optional acceleration must not consume memory needed to record. The
    // failed begin has joined its workers; release acceleration and retry once.
    s_previewDecode.close();
    s_cameraPreview.prepareDecoder(false,true);
    Serial.println("[camera-preview-worker] recording retry without acceleration");
    started=s_video.beginCamera(s_camera, width, height, s_shootMode == 2);
  }
#endif
  return started;
}

#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool runAudioTrial() {
  if (NavHandler::instance().top() != &app || camState != CAM_VIEWFINDER ||
      s_video.busy() || s_videoPending) return false;
  // Keep the ordinary Camera reservation and VGA preview during this trial.
  camStopViewfinder();
  s_shootMode = 2;
  if (!camStartViewfinder()) { showError(); return false; }
  return camBeginVideo();
}

bool runSettleTrial(uint32_t runId, int discardFrames, unsigned repeat) {
  if (NavHandler::instance().top() != &app || camState != CAM_VIEWFINDER || s_video.busy() || s_videoPending) {
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
  SR_VIDEO,
  SR_MANUAL,
  SR_COUNT
};

static int settingsRowCount() {
  return SR_COUNT - (s_settings.mode == CM_MANUAL ? 0 : 1) -
         (hal::camera::supportsAutofocus() ? 0 : 1);
}

// Visible indices omit autofocus when this sensor has no supported AF driver.
static SettingsRow settingsRowAt(int index) {
  if (!hal::camera::supportsAutofocus() && index >= SR_AUTOFOCUS) ++index;
  return static_cast<SettingsRow>(index);
}

static void camSettingsLabel(int i, char* buf, size_t cap) {
    switch (settingsRowAt(i)) {
        case SR_MODE:
            snprintf(buf, cap, "Camera mode  %s", kModeLabels[(int)s_settings.mode]);
            break;
        case SR_AUTOFOCUS:
            snprintf(buf, cap, "Autofocus  %s", s_settings.autofocus ? "ON" : "OFF");
            break;
        case SR_CAPTURE: {
            if (s_shootMode) {
                snprintf(buf,cap,"Capture  %s",camVideoHd()?"HD (720p)":"VGA (480p)");
                break;
            }
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
        case SR_VIDEO:
            snprintf(buf, cap, "Recording  %s", kShootLabels[s_shootMode]);
            break;
        default: buf[0] = 0; break;
    }
}

static ui::ListItem camSettingsItemFn(int idx, void*) {
    static char label[56];
    camSettingsLabel(idx, label, sizeof(label));
    ui::ListItem item;
    item.editing=s_editMode && idx==s_settingsSel; item.blinkOn=s_editBlink;
    item.opensScreen = settingsRowAt(idx) == SR_MANUAL;
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
  s_tutorialCapture=s_tutorialRequested;s_tutorialRequested=false;s_tutorialPhoto={};
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
  s_shootMode = s_tutorialCapture ? 0 : kDefaultShootMode;
  s_videoNoticeMs = 0;
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

// A long press latches recording; its release must not become a stop/photo tap.
static bool camShutterInput(hal::input::InputEvent ev) {
  using ET = hal::input::EventType;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  if (ev.type == ET::ButtonB_Press || ev.type == ET::ButtonB_Release || ev.type == ET::ButtonB_Tap)
    Serial.printf("[camera-shutter] event=%u armed=%u skip=%u quick=%u busy=%u\n",
        unsigned(ev.type),s_shutterArmed,s_shutterSkipTap,s_quickVideo,s_video.busy());
#endif
  if (ev.type == ET::ButtonB_Press) {
    s_shutterSkipTap = false;
    s_shutterArmed = camState == CAM_VIEWFINDER && !s_tutorialCapture &&
        !s_shootMode && !s_videoPending && !s_video.busy() && !s_reviewActive;
    s_shutterPressedMs = millis();
  } else if (ev.type == ET::ButtonB_Release) {
    s_shutterArmed = false;
  } else if (ev.type == ET::ButtonA_Press || ev.type == ET::ButtonC_Press) {
    if (s_shutterArmed) s_shutterSkipTap = true;
    s_shutterArmed = false;
  } else if (ev.type == ET::ButtonB_Tap) {
    s_shutterArmed = false;
    if (s_shutterSkipTap) { s_shutterSkipTap = false; return true; }
  }
  return false;
}

static void camTickShutter() {
  if (!s_shutterArmed) return;
  if (!hal::input::buttonBDown() || hal::input::buttonADown() || hal::input::buttonCDown()) {
    // Cancellation cannot turn a partial chord into a photo.
    if (hal::input::buttonADown() || hal::input::buttonCDown()) s_shutterSkipTap = true;
    s_shutterArmed = false;
    return;
  }
  if (uint32_t(millis()-s_shutterPressedMs) < kVideoHoldMs) return;
  s_shutterArmed = false;
  s_shutterSkipTap = true;
  s_quickVideo = true;
  s_shootMode = 2; // Quick video always uses the recorder's microphone processing.
  // Even VGA Photo needs video tuning/quality applied before the worker starts.
  s_continuousVga = false;
  if (!camBeginVideo() && !s_videoPending) camFinishQuickVideo();
  camPresentPhotographer();
}

static void camTick() {
  if(s_editMode && millis()-s_editBlinkMs>=450u) {
    s_editBlinkMs=millis(); s_editBlink=!s_editBlink;
    s_settingsNeedsRedraw=s_manualNeedsRedraw=true;
  }
  if (camState == CAM_VIEWFINDER) {
    camTickShutter();
    if (camState != CAM_VIEWFINDER) return;
    using VideoState = services::camera::VideoRecording::State;
    if (s_videoPending && !s_video.busy()) {
#if defined(ORIGAMI_ASYNC_CAMERA_PREVIEW)
      s_previewDecode.close();
#endif
      const bool saved = s_video.state() == VideoState::Saved;
      snprintf(s_videoNotice, sizeof(s_videoNotice), "%s", !saved ? s_video.error() :
          s_video.hasAudio() ? "VIDEO + MIC SAVED" : "VIDEO SAVED TO SD");
      camReportVideoResult();
      s_videoNoticeMs = millis();
      s_video.close();
      camFinishQuickVideo();
      s_videoPending = s_videoStopping = false;
      NavHandler::instance().runtimePolicyChanged();
      // close() acknowledges writer/microphone completion before sensor restart.
      // Keep the camera reservation while returning to the faster framing view.
      s_continuousVga = false;
      if (!camRestorePreviewAfterCapture(700)) return;
    }
    uint32_t videoLimit = services::camera::VideoRecording::kMaxDurationMs;
    if (s_video.busy() && millis() - s_videoStartedMs >= videoLimit) {
      s_video.stop(); s_videoStopping = true;
    }
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
    if (s_videoPending) {
#if (defined(ORIGAMI_VIDEO_HD_TRIAL) && !defined(ORIGAMI_VIDEO_HD_PREVIEW_TRIAL)) || defined(ORIGAMI_VIDEO_CAPTURE_ONLY_TRIAL)
      return; // No decoding/display traffic during the capture-only benchmark.
#endif
#if defined(ORIGAMI_ASYNC_CAMERA_PREVIEW)
      if (s_previewDecode.active()) {
        services::camera::PreviewDecode::Frame decoded{};
        const bool ready=s_previewDecode.acquire(decoded);
        if (ready) s_cameraPreview.copyDecoded(decoded.pixels);
        s_previewDecode.release();
        // Queue the next decode before transferring the completed image, so
        // CPU work on Core 0 can overlap the existing UI-owned panel transfer.
        hal::camera::CameraFrame source{};
        if (s_video.readPreview(&source)) {
          if (source.dmaStartUs!=s_previewSubmittedUs &&
              s_previewDecode.submit(source.buf,source.len,source.dmaStartUs))
            s_previewSubmittedUs=source.dmaStartUs;
          s_video.releasePreview();
        }
        if (ready) {
          const uint64_t before=esp_timer_get_time();
          camPresentPhotographer();s_lastFrameMs=millis();
          const uint32_t elapsed=uint32_t(esp_timer_get_time()-before);
          ++s_videoPerf.previews;s_videoPerf.previewUs+=elapsed;
          s_videoPerf.maxPreviewUs=std::max(s_videoPerf.maxPreviewUs,elapsed);
          if (s_videoPerf.previews%60==0) Serial.printf("[camera-preview-worker] frames=%lu elapsed_ms=%lu decode_us=%lu present_us=%lu age_us=%llu\n",
              (unsigned long)s_videoPerf.previews,(unsigned long)(millis()-s_videoStartedMs),
              (unsigned long)decoded.decodeUs,(unsigned long)elapsed,
              (unsigned long long)(esp_timer_get_time()-decoded.sensorUs));
        }
        return;
      }
#endif
      hal::camera::CameraFrame preview{};
      const uint32_t now=millis();
#if defined(ORIGAMI_REDUCED_VIDEO_DECODE_TRIAL)
      constexpr uint32_t previewIntervalMs=33;
#else
      const uint32_t previewIntervalMs=camVideoHd()?200:100;
#endif
      if (now-s_videoPreviewMs>=previewIntervalMs && s_video.readPreview(&preview)) {
        const uint64_t started=esp_timer_get_time();
        if (camRenderFrame(preview)) s_lastFrameMs=now;
        const uint32_t elapsed=uint32_t(esp_timer_get_time()-started);
        ++s_videoPerf.previews;s_videoPerf.previewUs+=elapsed;
        s_videoPerf.maxPreviewUs=std::max(s_videoPerf.maxPreviewUs,elapsed);
        s_video.releasePreview();s_videoPreviewMs=now;
      }
      return; // Acquisition and its timeout now belong to the recording worker.
    }
    hal::camera::CameraFrame fb;
    if (s_camera.read(&fb)) {
      if (camRenderFrame(fb)) s_lastFrameMs=millis();
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
  if (s_videoPending && s_video.hasAudio()) return; // Retain the microphone I2S route.
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
    switch (settingsRowAt(s_settingsSel)) {
      case SR_MODE: {
        int v = clampInt((int)s_settings.mode + steps, 0, CM_COUNT - 1);
        s_settings.mode = (CameraMode)v;
        s_settingsSel = clampInt(s_settingsSel, 0, settingsRowCount() - 1);
        break;
      }
      case SR_AUTOFOCUS:
        if(abs(steps)%2) s_settings.autofocus=!s_settings.autofocus;
        break;
      case SR_VIDEO:
        s_shootMode = ((s_shootMode + steps) % kShootModes + kShootModes) % kShootModes;
        break;
      case SR_CAPTURE: {
        // Allow the user to scroll to any preset. The "(low mem)" tag in the row label warns when
        // the current selection might not fit, and `camCaptureHighRes()` falls back to HD at
        // shutter time if the chosen preset can't allocate. (Earlier version clamped here, which
        // made the rotary feel dead at low PSRAM — looked like settings were broken.)
        int v = (int)s_settings.capture + steps;
        if (v < 0) v = 0;
        const int maximum=s_shootMode?int(CR_HD):CR_COUNT-1;
        if (v > maximum) v = maximum;
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
      if (s_video.busy()) { s_video.stop(); s_videoStopping = true; return; }
      NavHandler::instance().pop();
      break;
    case CAM_SETTINGS:
      if (s_editMode) {
        switch (settingsRowAt(s_settingsSel)) {
          case SR_MODE:       s_settings.mode      = (CameraMode)s_editBackup; break;
          case SR_AUTOFOCUS:  s_settings.autofocus       = (s_editBackup != 0); break;
          case SR_CAPTURE:    s_settings.capture     = (CapturePreset)s_editBackup; break;
          case SR_VIDEO:      s_shootMode = s_editBackup; break;
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

static void captureTutorialPhoto(){
 if(!services::photos::available()){snprintf(camStatusMsg,sizeof(camStatusMsg),"SD unavailable");showError();return;}
 const auto photo=services::photos::prepare();
 if(!photo.valid()){snprintf(camStatusMsg,sizeof(camStatusMsg),"Couldn't prepare photo");showError();return;}
 camShowShutterFrame();
 bool saved=false;
 if(s_continuousVga&&s_settings.capture==CR_VGA){
  services::camera::Snapshot snapshot;
  if(snapshot.captureVga(s_camera,esp_timer_get_time())==services::camera::Snapshot::Result::Ready){
   s_cameraStage=2;s_cameraStageAt=millis();s_cameraPulseAt=millis();camDrawPreview(snapshot.data(),snapshot.size());
   camDrawReviewStatus("Saving...");saved=services::photos::write(photo,snapshot.data(),snapshot.size());
  }
 }else{
  camReleaseDecoder();
  if(s_camera.prepareStill(s_settings.capture,s_settings.tuning())==services::camera::Session::StillResult::Ready)
   {camDrawReviewStatus("Saving...");saved=s_camera.captureToFile(photo.path);}
 }
 if(!saved){hal::sd::removeFile(photo.path);snprintf(camStatusMsg,sizeof(camStatusMsg),"Photo capture failed");showError();return;}
 camDrawReviewStatus("Saved");
 s_tutorialPhoto=photo;
 Serial.printf("[tutorial-photo] captured sequence=%lu queued=0\n",(unsigned long)photo.sequence);
 // App teardown stops the camera and schedules network restoration BEFORE
 // tutorial resume displays the still. No upload is admitted before Send.
 NavHandler::instance().pop();
}

static void camButtonB() {
  if(s_tutorialCapture&&camState==CAM_VIEWFINDER){captureTutorialPhoto();return;}
  switch (camState) {
    case CAM_VIEWFINDER: {
      if (s_video.busy()) { s_video.stop(); s_videoStopping = true; break; }
      if (s_shootMode) {
        camBeginVideo();
        camPresentPhotographer();
        break;
      }
      if (s_continuousVga && s_settings.capture == CR_VGA && s_camera.running()) {
        (void)camCaptureContinuousVga();
      } else {
        (void)camCaptureHighRes();
      }
      break;
    }
    case CAM_SETTINGS:
      if (!s_editMode) {
        if (settingsRowAt(s_settingsSel) == SR_MANUAL) {
          showManual();
          return;
        }
        s_editMode = true;
        switch (settingsRowAt(s_settingsSel)) {
          case SR_MODE:       s_editBackup = (int)s_settings.mode; break;
          case SR_AUTOFOCUS:  s_editBackup = s_settings.autofocus ? 1 : 0; break;
          case SR_CAPTURE:    s_editBackup = (int)s_settings.capture; break;
          case SR_VIDEO:      s_editBackup = s_shootMode; break;
          default:            s_editBackup = 0; break;
        }
        s_settingsNeedsRedraw = true; hal::display::markAllDirty();
      } else {
        persistTopLevelSetting(settingsRowAt(s_settingsSel));
        s_editMode = false;
        s_settingsNeedsRedraw = true; hal::display::markAllDirty();
        if (settingsRowAt(s_settingsSel) == SR_MODE && s_settings.mode == CM_MANUAL) {
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
  if (camShutterInput(ev)) return;
  using ET = hal::input::EventType;
  if (ui::nav::isBackTap(ev)) {
    camButtonA();
  } else if (camState == CAM_VIEWFINDER && ev.type == ET::ButtonB_Tap) {
    // Commit capture on release, only after HAL rules out an A+B chord.
    // A press can arrive before the other finger during a back gesture.
    camButtonB();
  } else if (ui::nav::isEnterTap(ev)) {
    if (camState == CAM_VIEWFINDER && s_video.busy()) {
      s_video.stop(); s_videoStopping = true;
    } else if (camState == CAM_VIEWFINDER) showSettings();
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
  // Root refresh can resume without a preceding cover; finalize before restart.
  if (s_video.busy()) s_video.close();
  if (app.currentScreen == &kScreenCameraViewfinder) (void)requestViewfinder();
  else if (app.currentScreen == &kScreenCameraSettings) prepareSettingsMode();
  else if (app.currentScreen == &kScreenCameraManual) prepareManualMode();
  else if (app.currentScreen == &kScreenCameraReadme) prepareReadmeMode();
}

static void coverCameraWorkflow() {
  if (camState == CAM_VIEWFINDER || s_camera.running()) camStopViewfinder();
  s_camera.restoreUi(services::camera::Session::Resume::Deferred);
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

static bool cameraChord(){return apps::tutorial::returnFromNavigation();}

static void viewfinderTick(uint32_t) {
  camTick();
}

const ::scene::Screen kScreenCameraViewfinder = {
    "Viewfinder",
    enterViewfinderScreen,
    nullptr,
    viewfinderTick,
    onInput,
    cameraChord,
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
    cameraChord,
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
    cameraChord,
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
    cameraChord,
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
    cameraChord,
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
void beginTutorialCapture(){s_tutorialRequested=true;NavHandler::instance().push(&app);}
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool copyPreviewForDevelopment(TFT_eSprite& target){return NavHandler::instance().top()==&app&&s_cameraPreview.copyCompositeForDevelopment(target);}
#endif
services::photos::Target takeTutorialPhoto(){auto photo=s_tutorialPhoto;s_tutorialPhoto={};return photo;}

static core::health::RuntimePolicy cameraRuntimePolicy() {
  return core::health::whileBusy(s_videoPending);
}

App app = {
  "Camera", &kScreenCameraViewfinder, nullptr, nullptr, camExit,
  "camera", "src/apps/camera_menu_app.cpp", kScreens,
  uint8_t(sizeof(kScreens) / sizeof(kScreens[0])), nullptr, cameraRuntimePolicy,
  camEnter, resumeCameraWorkflow, nullptr, coverCameraWorkflow
};

}  // namespace camera_menu
}  // namespace apps
