#include "apps/cine_camera_app.h"
#include "sfx/camera_shutter.h"
#include "sfx/recording.h"
#include "services/video_recording.h"
#include "services/camera_settings.h"
#include "services/uploads.h"
#include "apps/tutorial_app.h"
#include "services/cine_camera.h"
#include "core/cine_controls.h"
#include "core/haptic_catalog.h"
#include "services/vibration.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "ui/scene.h"
#include "ui/edit_field.h"
#include "ui/cine_camera_view.h"
#include "ui/draw.h"
#include "ui/nav_controls.h"
#include "ui/list_view.h"
#include "config/ui_theme.h"
#include <Arduino.h>
#include <stdio.h>
#include <string.h>
namespace apps {
namespace cine_camera {
namespace {
namespace cine = services::cine_camera;
cine::Session session;
services::camera::VideoRecording recorder;
services::camera::Settings cameraSettings;
bool microphone = true, clipActive = false, stopping = false;
bool transitionPulse = false, tutorialRequested = false, tutorialCapture = false;
uint32_t errorShownAt = 0;  // setup's first photo hands back after showing an open error
bool transitionFlushPending = false;
uint32_t transitionAt = 0, transitionPaintAt = 0, transitionPaintMaxUs = 0;
services::photos::Target tutorialPhoto;
uint32_t clipStartedAt = 0;
constexpr uint32_t kIdleReturnMs = 60000u;
uint32_t lastCameraActivityAt = 0;
core::cine::WheelControl wheel;
ui::ListView settingsList;
bool menuEditing = false;
bool shutterHeld = false, cancelTap = false, restoreAfterClip = false;
uint32_t shutterPressedAt = 0;
hal::camera::CineIspRequest clipSettings;
enum class View { Menu, Opening, Preview, Error };
View current = View::Menu;
unsigned feedbackStage = 0;
uint32_t feedbackAt = 0;
bool dirty = true;
uint32_t lastInputSample = 0;
// Keep the gap short between reduced-IDCT previews and input handling.
uint32_t lastPreview = 0;
constexpr uint16_t bg = 0xFFFF, fg = 0x0000, muted = 0x632C;
// Count completed new viewfinder images; input-only repaints do not advance FPS.
bool freshPreview = false;
uint32_t previewDecodeUs = 0;
struct PreviewRate {
  uint32_t start = 0, frames = 0, decode = 0, draw = 0;
  bool video = false;
  void frame(uint32_t paintUs) {
    const uint32_t now = millis();
    if (!start || video != clipActive || now - start > 6000) {
      start = now; frames = decode = draw = 0; video = clipActive;
      return;
    }
    ++frames; decode += previewDecodeUs; draw += paintUs;
    if (now - start >= 3000) {
      Serial.printf("[cine-rate] video=%u frames=%lu ms=%lu decode_us=%lu draw_us=%lu\n",
                    video, (unsigned long)frames, (unsigned long)(now-start),
                    (unsigned long)(decode/frames), (unsigned long)(draw/frames));
      start = now; frames = decode = draw = 0;
    }
  }
} previewRate;
void invalidate() {
  dirty = true;
  hal::display::markAllDirty();
}
bool finishClip() {
  recorder.close();
  const auto capture = recorder.captureStats();
  const auto writes = recorder.writerStats();
  Serial.printf("[cine-io] sensor_frames=%lu max_sd_chunk_ms=%lu max_queue=%lu\n",
                (unsigned long)capture.unique, (unsigned long)writes.maxChunkWriteMs,
                (unsigned long)recorder.submissionStats().maxQueued);
  Serial.printf("[cine-file] frames=%lu repeats=%lu audio_samples=%lu dropped=%lu\n",
                (unsigned long)recorder.frames(), (unsigned long)recorder.repeatedFrames(),
                (unsigned long)recorder.audioSamples(), (unsigned long)recorder.dropped());
  const bool saved = recorder.state() == services::camera::VideoRecording::State::Saved;
  if (saved && services::uploads::ensureInit())
    services::uploads::queue().enqueueVideo(recorder.path());
  Serial.printf("[camera-clip] saved=%u frames=%lu path=%s\n", saved,
                (unsigned long)recorder.frames(), recorder.path());
  clipActive = stopping = false;
  lastCameraActivityAt = millis();
  return saved;
}
bool close(bool keepReservation = false) {
  if (clipActive) finishClip();
  const bool ok = session.close(!keepReservation);
  current = ok ? View::Menu : View::Error;
  feedbackStage = 0;
  invalidate();
  return ok;
}
void begin();
void draw(TFT_eSprite &);
uint32_t openingPaintAt = 0;
void paintOpening() {
  const uint32_t now = millis();
  if (uint32_t(now - openingPaintAt) < 50u) return;
  openingPaintAt = now;
  invalidate();
  draw(hal::display::canvas());
  hal::display::flush();
}
void advanceShutter() {
  const uint32_t paintStarted = micros();
  if (transitionFlushPending) {
    // Rendering plus transfer can exceed the audio DMA window. Refill PCM
    // between those phases, using the display's existing canvas/scratch.
    hal::display::flush();
    transitionFlushPending = false;
  } else {
    if (!transitionPulse) return;
    const uint32_t now = millis();
    const bool expired = uint32_t(now - transitionAt) >= 140u;
    if (!expired && uint32_t(now - transitionPaintAt) < 28u) return;
    transitionPulse = !expired;
    transitionPaintAt = now;
    invalidate();
    draw(hal::display::canvas());
    transitionFlushPending = true;
  }
  const uint32_t paintedUs = micros() - paintStarted;
  if (paintedUs > transitionPaintMaxUs) transitionPaintMaxUs = paintedUs;
}
void presentCue(const sfx::PcmClip& cue, const char* label) {
  transitionPulse = true;
  transitionFlushPending = false;
  transitionAt = transitionPaintAt = millis();
  transitionPaintMaxUs = 0;
  invalidate();
  draw(hal::display::canvas());
  hal::display::flush();
  const uint32_t shown = micros();
  const bool played = sfx::playAndRelease(cue, advanceShutter);
  // Muted, busy or failed audio still gets the same short visual feedback.
  // Always remove the overlay before synchronous storage/sensor work begins.
  while (transitionPulse || transitionFlushPending) {
    advanceShutter();
    if (transitionPulse && !transitionFlushPending) delay(1);
  }
  Serial.printf("[camera-sfx] %s visual_us=%lu played=%u fade_ms=%lu paint_us=%lu\n", label,
                (unsigned long)shown, played, (unsigned long)(transitionPaintAt - transitionAt), (unsigned long)transitionPaintMaxUs);
}
void photoFeedback() {
  // The sensor frame is already secured: vibration cannot disturb this exposure.
  // Use the scroll tick as the initial photo feel, independent of audio volume.
  if (services::vibration::hapticsEnabled())
    services::vibration::playEffect(core::haptic::catalog::scroll);
  presentCue(sfx::kCameraShutter, "photo");
}
void activate() {
  lastCameraActivityAt = millis();
  tutorialCapture = tutorialRequested;
  tutorialRequested = false;
  tutorialPhoto = {};
  cameraSettings.load();
  microphone = true;
  restoreAfterClip = false;
  feedbackStage = 0;
  begin();
}
void exit() {
  close();
}
void cover() {
  shutterHeld = false;
  if (current == View::Preview) {
    clipSettings = session.captureSettings();
    restoreAfterClip = true;
  }
  close();
}
void resume() {
  lastCameraActivityAt = millis();
  if (restoreAfterClip) {
    current = View::Opening;
    invalidate();
  } else invalidate();
}
void begin() {
  previewRate = {};
  freshPreview = false;
  wheel.reset();
  current = View::Opening;
  invalidate();
  openingPaintAt = millis() - 50u;
  paintOpening();
}
void snap() {
  if (current != View::Preview)
    return;
  // Capture owns its feedback. Do not append the navigator's generic Select
  // cue after this synchronous operation, including when capture fails.
  services::vibration::suppressInputFeedback();
  // Secure the exposure before presenting the synchronized shutter feedback.
  const bool saved = session.capture(photoFeedback, !tutorialCapture);
  lastCameraActivityAt = millis();
  Serial.printf("[camera-photo] saved=%u queued=%u path=%s\n", saved, session.status().queued, session.status().photoPath);
  if (!saved) Serial.printf("[camera-error] capture: %s\n", session.status().message);
  if (saved && tutorialCapture) {
    const auto& result = session.status();
    tutorialPhoto.sequence = result.sequence;
    tutorialPhoto.capturedAtUnix = result.capturedAtUnix;
    snprintf(tutorialPhoto.path, sizeof(tutorialPhoto.path), "%s", result.photoPath);
    Serial.printf("[tutorial-photo] captured sequence=%lu queued=0\n", (unsigned long)result.sequence);
    NavHandler::instance().pop(); // Exit releases hardware before tutorial resumes.
    return;
  }
  feedbackStage = saved ? (session.status().queued ? 5 : 8) : 6;
  feedbackAt = millis();
  invalidate();
  draw(hal::display::canvas());
  hal::display::flush();
}
bool previewShouldAbort(void *) {
  // Match stock Camera: sample/enqueue only, never dispatch with a frame loan.
  const uint32_t now = millis();
  // Expander reads are synchronous I2C: keep taps responsive without delaying a frame.
  if (uint32_t(now - lastInputSample) >= 20u) {
    lastInputSample = now;
    hal::input::service(now);
  }
  return hal::input::buttonADown() || hal::input::buttonBDown() || hal::input::buttonCDown();
}
void restoreStill(bool saved) {
  current = session.open() && session.restoreSettings(clipSettings) ? View::Preview : View::Error;
  feedbackStage = saved ? 7 : 6;
  feedbackAt = millis();
  invalidate();
}
bool beginVideo() {
  if (current != View::Preview || tutorialCapture || clipActive || recorder.busy()) return false;
  const uint32_t requestedAt = millis();
  clipSettings = session.captureSettings();
  cancelTap = true;
  feedbackStage = 0;
  clipActive = true;
  clipStartedAt = millis();
  presentCue(sfx::kRecordingStart, "start");
  const uint32_t cueDoneAt = millis();
  const bool hd = cameraSettings.capture != services::camera::CaptureResolution::Vga;
  if (!session.beginClip(recorder, hd, microphone)) {
    recorder.close();
    clipActive = false;
    restoreStill(false);
  }
  Serial.printf("[video-start] press_ms=%lu request_ms=%lu cue_done_ms=%lu ready_ms=%lu accepted=%u\n",
                (unsigned long)shutterPressedAt, (unsigned long)requestedAt,
                (unsigned long)cueDoneAt, (unsigned long)millis(), clipActive);
  invalidate();
  return clipActive;
}
void tick(uint32_t) {
  const uint32_t now = millis();
  if (clipActive) {
    if (!recorder.busy()) {
      const bool saved = finishClip();
      if (saved) presentCue(sfx::kRecordingStop, "stop");
      restoreStill(saved);
    } else {
      if (!stopping && now - clipStartedAt >= services::camera::VideoRecording::kMaxDurationMs) {
        stopping = true;
        recorder.stop();
      }
      const uint32_t at = micros();
      if (session.previewClip(recorder)) {
        previewDecodeUs = micros()-at; freshPreview = true; invalidate();
      }
    }
    return;
  }
  // Setup's first photo returns to setup, which reports to the phone; popping
  // to the root would leave onboarding mid-way.
  if (tutorialCapture && current == View::Error && uint32_t(now - errorShownAt) >= 3000u) {
    Serial.println("[camera-error] returning to setup");
    NavHandler::instance().pop();
    return;
  }
  if (uint32_t(now - lastCameraActivityAt) >= kIdleReturnMs) {
    Serial.printf("[camera-idle] returning %s after 60s without input\n", tutorialCapture ? "to setup" : "home");
    if (tutorialCapture) NavHandler::instance().pop();
    else NavHandler::instance().popToRoot();
    return;
  }
  if (shutterHeld && !tutorialCapture && current == View::Preview) {
    if (hal::input::buttonADown() || hal::input::buttonCDown()) {
      shutterHeld = false;
      cancelTap = true;
    } else if (hal::input::buttonBDown() && now - shutterPressedAt >= 1200u) {
      shutterHeld = false;
      beginVideo();
      return;
    }
  }
  if (wheel.expire(now))
    invalidate();
  if (feedbackStage && feedbackStage != 1 && uint32_t(now - feedbackAt) >= 1800u) {
    feedbackStage = 0;
    invalidate();
  }
  if (current == View::Opening) {
    paintOpening();
    const auto preparation = session.prepareOpen();
    if (preparation == cine::Session::Preparation::Pending)
      return;
    current = preparation == cine::Session::Preparation::Ready && session.open(paintOpening)
                  ? View::Preview : View::Error;
    if (restoreAfterClip && current == View::Preview && !session.restoreSettings(clipSettings))
      current = View::Error;
    restoreAfterClip = false;
    if (current == View::Error) {
      Serial.printf("[camera-error] open: %s\n", session.status().message);
      session.close();
      errorShownAt = now;
    }
    lastPreview = millis();
    invalidate();
  } else if (current == View::Preview && uint32_t(now - lastPreview) >= 1u) {
    const uint32_t at = micros();
    if (session.preview(previewShouldAbort)) {
      previewDecodeUs = micros()-at; freshPreview = true; invalidate();
    }
    lastPreview = millis();
  }
}
void shutterValue(char *text, size_t bytes) {
  if (session.autoExposure()) {
    snprintf(text, bytes, "Auto");
    return;
  }
  if (session.shutter().valid && session.shutter().actualUs)
    snprintf(
        text, bytes, "1/%lu",
        (unsigned long)((1000000 + session.shutter().actualUs / 2) / session.shutter().actualUs));
  else
    snprintf(text, bytes, "Auto");
}
ui::ListItem settingsItem(int index, void *) {
  static char label[64];
  char shutter[20];
  shutterValue(shutter, sizeof(shutter));
  if (index == 0)
    snprintf(label, sizeof(label), "Shutter  %s", shutter);
  else if (index == 1)
    snprintf(label, sizeof(label), "Color  %s",
             cine::Session::colorName(session.colorMode()));
  else if (index == 2)
    snprintf(label, sizeof(label), "Video  %s", cameraSettings.capture == services::camera::CaptureResolution::Vga ? "480p" : "720p");
  else if (index == 3)
    snprintf(label, sizeof(label), "Microphone  %s", microphone ? "On" : "Off");
  else
    snprintf(label, sizeof(label), "Back to camera");
  ui::ListItem item;
  item.label = label;
  item.fg = fg;
  item.opensScreen = index == 4;
  item.editing = menuEditing && index == settingsList.selected;
  return item;
}
const char *clipCaption() {
  if (!clipActive) return nullptr;
  if (stopping) return "SAVING VIDEO";
  static char caption[32];
  const unsigned secs = (millis() - clipStartedAt) / 1000;
  snprintf(caption, sizeof(caption), "REC %u:%02u  B: STOP", secs / 60, secs % 60);
  return caption;
}
void build(ui::scene::Scene &view, int16_t w, int16_t h) {
  view.clear();
  ui::scene::setCurrentScene(&view);
  if (current == View::Preview) {
    view.addRect("cine.preview", {0, 0, w, h}, 0);
    auto *gear = view.addBitmap("cine.settings_button", {18, 14, 20, 20},
                                ui::cine_view::settingsMask, 20, 20, 0xffff, 0);
    if (gear)
      gear->bitmapTransparent = true;
    view.addLabel("cine.controls", {52, 196, 200, 20}, clipActive ? clipCaption() : (tutorialCapture ? "B: PHOTO" : "B: PHOTO / HOLD: VIDEO"), 0xffff, 0, 1);
    if (wheel.shown(millis())) {
      const uint16_t ss = wheel.color() ? 0xffff : kUiEdit,
                     color = wheel.color() ? kUiEdit : 0xffff;
      char shutter[20];
      shutterValue(shutter, sizeof(shutter));
      view.addLabel("cine.wheel_shutter_label", {142, 82, 26, 12}, "SS", ss, 0, 1);
      view.addLabel("cine.wheel_color_label", {130, 52, 42, 12}, "Color", color, 0, 1);
      view.addLabel("cine.wheel_shutter_value", {178, 76, 90, 26}, shutter, ss, 0, 2);
      view.addLabel("cine.wheel_color_value", {180, 46, 90, 26},
                    cine::Session::colorName(session.colorMode()), color, 0, 2);
    }
    if (feedbackStage)
      view.addLabel("cine.feedback", {52, 196, 180, 20},
                    feedbackStage == 1   ? "CAPTURE"
                    : feedbackStage == 7 ? "VIDEO SAVED"
                    : feedbackStage == 5 ? "PHOTO SAVED"
                    : feedbackStage == 8 ? "SAVED / NOT QUEUED"
                                         : "SAVE FAILED",
                    0xffff, 0, 1);
    return;
  }
  view.addRect("cine.bg", {0, 38, w, (int16_t)(h - 38)}, bg);
  if (current == View::Opening) {
    view.addLabel("cine.opening", {8, 96, (int16_t)(w - 16), 44},
                  "Opening camera", fg, bg, 3, ui::scene::TextAlign::MiddleCenter);
    for (int i = 0; i < 3; ++i) {
      const unsigned phase = (millis() / 50u + 24u - unsigned(i) * 5u) % 24u;
      const int rise = phase < 6 ? phase : phase < 12 ? 12 - phase : 0;
      char id[24];
      snprintf(id, sizeof(id), "cine.opening.dot%d", i);
      view.addRect(id, {(int16_t)(w / 2 - 24 + i * 20), (int16_t)(157 - rise), 8, 8}, fg, 4);
    }
    return;
  }
  if (current == View::Menu) {
    settingsList.totalItems = 5;
    settingsList.buildScene(view, 0, 38, w, h - 38, settingsItem, nullptr, kUiSelBg, kUiSelFg, bg,
                            muted, fg);
    return;
  }
  view.addLabel("cine.title", {0, 55, w, 26}, "Camera", fg, bg, 2,
                ui::scene::TextAlign::MiddleCenter);
  view.addLabel("cine.status", {8, 96, (int16_t)(w - 16), 50},
                session.status().message, fg, bg,
                1, ui::scene::TextAlign::MiddleCenter);
  view.addLabel("cine.controls", {0, 212, w, 18}, "A: back", muted, bg, 1,
                ui::scene::TextAlign::MiddleCenter);
}
struct PreviewCanvas {
  TFT_eSprite &surface;
  uint16_t at(int x, int y) const { return surface.readPixel(x, y); }
  uint16_t readPixel(int x, int y) const { return surface.readPixel(x, y); }
  void fillRect(int x, int y, int w, int h, uint16_t ink) { surface.fillRect(x, y, w, h, ink); }
  void pushImage(int x, int y, int w, int h, uint16_t *pixels) {
    const bool swap = surface.getSwapBytes();
    surface.setSwapBytes(true);
    surface.pushImage(x, y, w, h, pixels);
    surface.setSwapBytes(swap);
    (void)previewShouldAbort(nullptr);
  }
};
void draw(TFT_eSprite &canvas) {
  if (!dirty)
    return;
  auto &view = ui::scene::scratchScene();
  build(view, canvas.width(), canvas.height());
  if (current == View::Preview) {
    const uint32_t at = micros();
    PreviewCanvas target{canvas};
    ui::cine_view::image(target, session.pixels(), cine::Session::previewWidth,
                         cine::Session::previewHeight, transitionPulse ? ui::cine_view::shutterAlpha(millis() - transitionAt) : 0);
    char shutter[20];
    shutterValue(shutter, sizeof(shutter));
    ui::cine_view::chrome(target, cine::Session::colorName(session.colorMode()), shutter,
                          feedbackStage * 1000 +
                              (millis() - feedbackAt < 999 ? millis() - feedbackAt : 999),
                          wheel.color(), !clipActive && wheel.shown(millis()), clipCaption());
    if (freshPreview) previewRate.frame(micros()-at);
    freshPreview = false;
  } else
    view.render(canvas);
  dirty = false;
}
AppPresentation presentation() {
  return current == View::Preview ? AppPresentation{true, true, AppPresentation::StatusBar::Hidden}
                                  : AppPresentation{};
}

void input(hal::input::InputEvent e) {
  using ET = hal::input::EventType;
  if (e.type == ET::ButtonA_Tap && current == View::Preview) {
    if (tutorialCapture) { NavHandler::instance().pop(); return; }
    current = View::Menu;
    menuEditing = false;
    settingsList.totalItems = 5;
    settingsList.selected = wheel.color() ? 1 : 0;
    invalidate();
    return;
  }
  if (current == View::Menu) {
    if (ui::nav::isBackTap(e)) {
      if (menuEditing)
        menuEditing = false;
      else if (session.status().open)
        current = View::Preview;
      else
        begin();
      invalidate();
      return;
    }
    if (e.type == ET::Scroll) {
      if (menuEditing) {
        if (settingsList.selected == 0)
          session.adjustShutter(-int(e.delta));
        else if (settingsList.selected == 1)
          session.adjustColor(int(e.delta));
        else if (settingsList.selected == 2 && e.delta)
          cameraSettings.capture = e.delta > 0 ? services::camera::CaptureResolution::Hd : services::camera::CaptureResolution::Vga;
        else if (settingsList.selected == 3 && e.delta)
          microphone = e.delta > 0;
        if (!session.status().open)
          current = View::Error;
      } else
        settingsList.scroll(e.delta);
      invalidate();
    } else if (ui::nav::isEnterTap(e)) {
      if (settingsList.selected == 4) {
        menuEditing = false;
        if (session.status().open)
          current = View::Preview;
        else
          begin();
      } else {
        if (menuEditing && settingsList.selected == 2)
          cameraSettings.save(services::camera::Setting::Capture);
        menuEditing = !menuEditing;
      }
      invalidate();
    }
    return;
  }
  if (ui::nav::isBackTap(e)) {
    NavHandler::instance().pop();
    return;
  }
  if (current != View::Preview)
    return;
  if (e.type == ET::ButtonB_Tap)
    snap();
  else if (ui::nav::isEnterTap(e)) {
    services::vibration::navigationFeedback(services::vibration::Cue::Select);
    wheel.toggle(millis());
    invalidate();
  } else if (e.type == ET::Scroll) {
    if (!e.delta)
      return;
    if (wheel.color())
      session.adjustColor(int(e.delta));
    else
      session.adjustShutter(-int(e.delta));
    wheel.show(millis());
    if (!session.status().open)
      current = View::Error;
    invalidate();
  }
}
void shutterInput(hal::input::InputEvent e) {
  using ET = hal::input::EventType;
  lastCameraActivityAt = millis();
  if (e.type == ET::ButtonB_Press) {
    shutterHeld = current == View::Preview;
    cancelTap = false;
    shutterPressedAt = millis();
  } else if (e.type == ET::ButtonB_Release) {
    shutterHeld = false;
  } else if (shutterHeld && (e.type == ET::ButtonA_Press || e.type == ET::ButtonC_Press)) {
    shutterHeld = false;
    cancelTap = true;
  } else if (e.type == ET::ButtonB_Tap && cancelTap) {
    cancelTap = false;
    return;
  }
  if (clipActive) {
    shutterHeld = false;
    if (e.type == ET::ButtonB_Tap && !stopping) {
      stopping = true;
      recorder.stop();
      invalidate();
    }
    return;
  }
  input(e);
}
bool holdWheel() {
  if (clipActive) return true;
  // In every Cine view this gesture is consumed instead of putting Kami to sleep.
  if (session.status().open && session.resumeAutoExposure()) {
    wheel.reset(false); // Restoring Auto explicitly selects its shutter readout.
    wheel.show(millis());
    settingsList.selected = 0;
    menuEditing = false;
    services::vibration::navigationFeedback(services::vibration::Cue::Select);
    invalidate();
  }
  return true;
}
core::health::RuntimePolicy policy() {
  bool busy = current == View::Preview || current == View::Opening || session.holdsResources();
  auto p = core::health::whileBusy(busy);
  p.allowBackgroundUploads = !busy;
  p.allowAutomaticUpdate = !busy;
  return p;
}
bool chord() { return apps::tutorial::returnFromNavigation(); }
const ::scene::Screen screen = {"Camera", invalidate, nullptr, tick,  shutterInput,
                                chord,         draw,       "main",  build, &bg};
const ::scene::Screen *const screens[] = {&screen};
} // namespace
void beginTutorialCapture() { tutorialRequested = true; NavHandler::instance().push(&app); }
services::photos::Target takeTutorialPhoto() { auto result = tutorialPhoto; tutorialPhoto = {}; return result; }
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool runAudioTrial() { return NavHandler::instance().top() == &app && beginVideo(); }
#endif
App app = [] {
  App a = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Camera", "cine-camera", &screen, activate,
                                            resume, exit, "src/apps/cine_camera_app.cpp",
                                            screens, presentation, policy);
  a.onCover = cover;
  a.onPowerHold = holdWheel;
  return a;
}();
} // namespace cine_camera
} // namespace apps
