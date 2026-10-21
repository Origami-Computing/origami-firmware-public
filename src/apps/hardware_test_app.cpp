#include "config/screen_chrome_layout.h"
/**
 * Hardware self-test: Display, Button A, Button B, Vibration, SD Card,
 * Audio, Camera, IMU Level, Recording, Complete.
 *
 * App callbacks own workflow activation/resume/teardown. The screen composes
 * cached instructions/results; tick and input handlers perform the tests.
 * Tone, RawRecording and camera Session own their operational resources.
 *
 * Raw motor writes are an intentional diagnostic exception: tick follows held A;
 * release, retreat and app exit write false without claiming a vibration owner.
 * A service wrapper would add no session and owner-guarded cleanup would change
 * behavior. SD and IMU queries observe existing drivers without acquiring them.
 *
 * A advances completed steps except Vibration and Audio; those currently have
 * no forward transition. B confirms Button B, otherwise retreats (or exits at
 * Display/Complete). Keep this existing continuation defect separate from the
 * ownership refactor. Tone writes retain their blocking portMAX_DELAY policy.
 */

#include "apps/hardware_test_app.h"

#include <Arduino.h>
#include "app.h"
#include "services/diagnostic_recording.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include <cmath>
#include <cstring>
#include <cstdio>

#include "Pins.h"
#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "services/diagnostic_tone.h"
#include "hal/hal_audio.h"
#include "services/camera_session.h"
#include "hal/hal_display.h"
#include "hal/hal_imu.h"
#include "hal/hal_input.h"
#include "hal/hal_io.h"
#include "hal/hal_sd.h"
#include "ui/scene.h"
#include "ui/source_layout.h"

namespace apps {
namespace hardware_test {

// ---- Test step definitions --------------------------------------------------

enum TestStep {
  TEST_DISPLAY = 0, TEST_BUTTON_A, TEST_BUTTON_B, TEST_VIBRATION,
  TEST_SD_CARD, TEST_AUDIO_TONE, TEST_CAMERA, TEST_IMU,
  TEST_RECORDING, TEST_COMPLETE
};
static constexpr int kStepCount = TEST_COMPLETE + 1;

static const char* kStepNames[] = {
  "Display", "Button A", "Button B", "Vibration",
  "SD Card", "Audio (440Hz)", "Camera", "IMU Level",
  "Recording", "Complete!"
};

// ---- Step state -------------------------------------------------------------

static int       s_step         = 0;
static bool      s_stepComplete = false;
static bool      s_buttonAHeld  = false;
static bool      s_needRedraw   = false;
static uint32_t  s_lastUpdateMs = 0;

// ---- Draw state — set by business logic, consumed by onDraw -----------------

static char s_instr[80];
static char s_instr2[80];

static bool    s_resultVisible = false;
static char    s_resultText[64];
static bool    s_resultPass    = false;

// Display color test
static bool     s_hasColorBox  = false;
static uint16_t s_colorBoxCol  = 0;
static char     s_colorBoxLbl[16];
static int      s_colorIdx     = 0;

// IMU bubble
static bool     s_hasBubble   = false;
static int      s_bubOffX     = 0, s_bubOffY = 0;
static uint16_t s_bubColor    = 0;
static char     s_imuOrient[24];

// Recording status
static float s_recDuration    = 0.0f;
static bool  s_hasRecStatus   = false;
static char  s_recStatus[24];

// ---- Audio tone -------------------------------------------------------------

static bool  s_toneActive = false;
static services::diagnostics::Tone s_tone;

// ---- Camera / recording state -----------------------------------------------

static bool     s_cameraReady = false;
static services::camera::Session s_camera;
static uint32_t s_photoCount  = 0;

static bool           s_isRecording = false;
static services::diagnostics::RawRecording s_recording;
static uint32_t s_recFileIdx  = 0;

// ---- Helpers ----------------------------------------------------------------

static constexpr int kColorCount = 7;
static const uint16_t kColors[kColorCount] = {
  TFT_RED, TFT_GREEN, TFT_BLUE, TFT_WHITE, TFT_YELLOW, TFT_MAGENTA, TFT_CYAN
};
static const char* kColorNames[kColorCount] = {
  "RED","GREEN","BLUE","WHITE","YELLOW","MAGENTA","CYAN"
};

static void updateInstructions() {
  s_instr[0] = s_instr2[0] = '\0';
  switch (s_step) {
    case TEST_DISPLAY:   strlcpy(s_instr, "A: Cycle colors", sizeof(s_instr)); break;
    case TEST_BUTTON_A:  strlcpy(s_instr, "Press Button A...", sizeof(s_instr)); break;
    case TEST_BUTTON_B:
      strlcpy(s_instr,  "Press Button B to confirm", sizeof(s_instr));
      strlcpy(s_instr2, "(will also advance)", sizeof(s_instr2));
      break;
    case TEST_VIBRATION: strlcpy(s_instr, "Hold A to vibrate", sizeof(s_instr)); break;
    case TEST_SD_CARD:   strlcpy(s_instr, "Checking SD card...", sizeof(s_instr)); break;
    case TEST_AUDIO_TONE: strlcpy(s_instr, "Hold A to play 440Hz tone", sizeof(s_instr)); break;
    case TEST_CAMERA:
      strlcpy(s_instr,  "Initializing camera...", sizeof(s_instr));
      strlcpy(s_instr2, "A: Take photo  B: Back", sizeof(s_instr2));
      break;
    case TEST_IMU:   strlcpy(s_instr, "Tilt device - bubble level", sizeof(s_instr)); break;
    case TEST_RECORDING:
      strlcpy(s_instr,  "Hold A to record audio", sizeof(s_instr));
      strlcpy(s_instr2, "Saves WAV to SD card", sizeof(s_instr2));
      break;
    case TEST_COMPLETE: strlcpy(s_instr, "All tests complete!", sizeof(s_instr)); break;
  }
}

static void clearDynamic() {
  s_resultVisible = false;
  s_hasColorBox   = false;
  s_hasBubble     = false;
  s_imuOrient[0]  = '\0';
  s_hasRecStatus  = false;
  s_recDuration   = 0.0f;
}

static void setResult(const char* text, bool pass) {
  strlcpy(s_resultText, text, sizeof(s_resultText));
  s_resultPass    = pass;
  s_resultVisible = true;
  s_stepComplete  = pass;
  s_needRedraw    = true;
  hal::display::markAllDirty();
}

// ---- onDraw -----------------------------------------------------------------

static void buildHardwareScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  ui::scene::Builder b(scene, width, height);
  b.clearContent(kUiBg);

  char hdr[32];
  snprintf(hdr, sizeof(hdr), "Test %d/%d", s_step + 1, kStepCount);
  b.panelHeader("hardware.header", hdr);
  scene.addLabel("hardware.step", {10, (int16_t)(b.curY() + 2), (int16_t)(width - 20), 18},
                 kStepNames[s_step], kUiFg, kUiBg, 2, ui::scene::TextAlign::TopLeft, 1);
  b.spacer(20);
  if (s_instr[0])  b.statusLine("hardware.instr.0", s_instr, kUiMuted);
  if (s_instr2[0]) b.statusLine("hardware.instr.1", s_instr2, kUiMuted);
  b.spacer(4);

  // Display color box (TEST_DISPLAY)
  if (s_hasColorBox) {
    const int bx = 10, bw = width - 20, bh = 46;
    const int y = b.curY();
    scene.addRect("hardware.color.bg", {bx, (int16_t)y, (int16_t)bw, (int16_t)bh}, s_colorBoxCol, 0, 1);
    scene.addLabel("hardware.color.label", {bx, (int16_t)y, (int16_t)bw, (int16_t)bh}, s_colorBoxLbl,
                   s_colorBoxCol == 0xFFFF ? kUiFg : TFT_WHITE, s_colorBoxCol,
                   2, ui::scene::TextAlign::MiddleCenter, 2);
    b.spacer(bh + 4);
  }

  // IMU bubble level (TEST_IMU)
  if (s_step == TEST_IMU) {
    const int cx = width / 2;
    const int cy = b.curY() + 52;
    scene.addCircle("hardware.imu.outer", cx, cy, 50, kUiFg, 1, kUiBg, false, 1);
    scene.addLine("hardware.imu.cross.x", cx - 10, cy, cx + 10, cy, kUiMuted, 1);
    scene.addLine("hardware.imu.cross.y", cx, cy - 10, cx, cy + 10, kUiMuted, 1);
    if (s_hasBubble) {
      scene.addCircle("hardware.imu.bubble.fill", cx + s_bubOffX, cy + s_bubOffY, 8,
                      kUiFg, 1, s_bubColor, true, 2);
    }
    b.spacer(112);
    if (s_imuOrient[0]) b.labelCenterMuted("hardware.imu.orientation", s_imuOrient);
  }

  // Recording status
  if (s_hasRecStatus) {
    char line[64];
    snprintf(line, sizeof(line), "%s  %.1fs", s_recStatus, s_recDuration);
    b.statusLine("hardware.recording.status", line, kUiFg);
  }

  // Result
  if (s_resultVisible) {
    if (s_resultPass) {
      b.statusLine("hardware.result", s_resultText, 0x07E0);
    } else {
      b.statusLine("hardware.result", s_resultText, TFT_RED);
    }
    if (s_stepComplete) b.statusLine("hardware.continue", "Press A to continue", kUiMuted);
  }

  ui::scene::applySourceLayout(scene, "hardware", "test");
}

void onDraw(TFT_eSprite& canvas) {
  if (!s_needRedraw) return;
  s_needRedraw = false;

  ui::scene::Scene& scene = ui::scene::scratchScene();
  buildHardwareScene(scene, canvas.width(), canvas.height());
  scene.render(canvas);
  hal::display::markAllDirty();
}

// ---- Tick -------------------------------------------------------------------

void tick() {
  const uint32_t now = millis();

  if (s_step == TEST_VIBRATION) {
    hal::io::setVibration(s_buttonAHeld);
    if (s_buttonAHeld && !s_stepComplete) {
      s_stepComplete = true;
      s_needRedraw = true;
      hal::display::markAllDirty();
    }
  }

  if (s_step == TEST_AUDIO_TONE && s_buttonAHeld) {
    s_tone.writeBlock();
    if (!s_stepComplete) {
      s_stepComplete = true;
      s_needRedraw   = true;
      hal::display::markAllDirty();
    }
  }

  if (s_step == TEST_RECORDING && s_isRecording && s_buttonAHeld) {
    s_recording.drain();
    if ((uint32_t)(now - s_lastUpdateMs) > 200u) {
      s_recDuration  = (float)s_recording.samples() / 48000.0f;
      s_hasRecStatus = true;
      strlcpy(s_recStatus, "RECORDING", sizeof(s_recStatus));
      s_needRedraw   = true;
      hal::display::markAllDirty();
      s_lastUpdateMs = now;
    }
  }

  switch (s_step) {
    case TEST_SD_CARD:
      if ((uint32_t)(now - s_lastUpdateMs) > 500u) {
        if (hal::sd::mounted()) {
          uint8_t  ct = SD.cardType();
          uint64_t sz = SD.cardSize() / (1024*1024);
          char buf[48];
          snprintf(buf, sizeof(buf), "Type:%d Size:%lluMB", ct, (unsigned long long)sz);
          setResult(buf, true);
        } else {
          setResult("SD not mounted", false);
        }
        s_lastUpdateMs = now;
      }
      break;

    case TEST_CAMERA:
      if (!s_cameraReady && (uint32_t)(now - s_lastUpdateMs) > 500u) {
        if (s_camera.start()) {
          s_cameraReady = true;
          strlcpy(s_instr,  "Camera ready!", sizeof(s_instr));
          strlcpy(s_instr2, "A: Take photo  B: Back", sizeof(s_instr2));
        } else {
          strlcpy(s_instr,  "Camera init failed", sizeof(s_instr));
          s_instr2[0] = '\0';
        }
        s_needRedraw = true;
        hal::display::markAllDirty();
        s_lastUpdateMs = now;
      }
      break;

    case TEST_IMU:
      if ((uint32_t)(now - s_lastUpdateMs) > 50u) {
        int32_t accel[3], gyro[3];
        if (hal::imu::readAxes(accel, gyro)) {
          int ox = (int)(accel[0] * 0.04f);
          int oy = (int)(accel[1] * 0.04f);
          const float maxOff = 40.0f;
          float dist = sqrtf((float)(ox*ox + oy*oy));
          if (dist > maxOff) { float sc = maxOff/dist; ox = (int)(ox*sc); oy = (int)(oy*sc); }
          s_bubOffX = ox; s_bubOffY = oy;
          float tilt = sqrtf((float)(accel[0]*accel[0] + accel[1]*accel[1]));
          s_bubColor = (tilt < 100) ? (uint16_t)TFT_GREEN :
                       (tilt < 300) ? (uint16_t)TFT_YELLOW : (uint16_t)TFT_RED;
          s_hasBubble = true;
          int32_t ax = abs(accel[0]), ay = abs(accel[1]), az = abs(accel[2]);
          if      (az > 800)           strlcpy(s_imuOrient, accel[2] > 0 ? "FLAT" : "UPSIDE DOWN", sizeof(s_imuOrient));
          else if (ax > ay && ax > 500) strlcpy(s_imuOrient, accel[0] > 0 ? "TILT RIGHT" : "TILT LEFT", sizeof(s_imuOrient));
          else if (ay > 500)            strlcpy(s_imuOrient, accel[1] > 0 ? "TILT FWD" : "TILT BACK", sizeof(s_imuOrient));
          else                          strlcpy(s_imuOrient, "ANGLED", sizeof(s_imuOrient));
          if (!s_stepComplete) s_stepComplete = true;
          s_needRedraw = true;
          hal::display::markAllDirty();
        }
        s_lastUpdateMs = now;
      }
      break;

    default: break;
  }
}

// ---- Step navigation --------------------------------------------------------

static void stepAdvance() {
  if (s_step >= kStepCount - 1) return;
  if (s_step == TEST_CAMERA && s_cameraReady) { s_camera.stop(); s_cameraReady = false; }
  s_step++;
  s_lastUpdateMs = 0;
  s_toneActive   = false;
  s_stepComplete = false;
  clearDynamic();
  updateInstructions();
  s_needRedraw = true;
  hal::display::markAllDirty();
}

static void stepRetreat() {
  if (s_step <= TEST_DISPLAY) return;
  if (s_step == TEST_CAMERA && s_cameraReady) { s_camera.stop(); s_cameraReady = false; }
  if (s_step == TEST_RECORDING && s_isRecording) {
    s_isRecording = false;
    (void)s_recording.finish();
  }
  hal::io::setVibration(false);
  s_toneActive = false;
  s_tone.stop();
  s_step--;
  s_lastUpdateMs = 0;
  s_stepComplete = false;
  clearDynamic();
  updateInstructions();
  s_needRedraw = true;
  hal::display::markAllDirty();
}

// ---- Input ------------------------------------------------------------------

static void onButtonAPress() {
  if (s_stepComplete && s_step != TEST_VIBRATION && s_step != TEST_AUDIO_TONE) {
    stepAdvance(); return;
  }
  switch (s_step) {
    case TEST_DISPLAY:
      s_hasColorBox  = true;
      s_colorBoxCol  = kColors[s_colorIdx];
      strlcpy(s_colorBoxLbl, kColorNames[s_colorIdx], sizeof(s_colorBoxLbl));
      s_colorIdx = (s_colorIdx + 1) % kColorCount;
      if (s_colorIdx == 0) setResult("Display OK - all colors shown", true);
      else { s_needRedraw = true; hal::display::markAllDirty(); }
      break;
    case TEST_BUTTON_A:
      setResult("Button A works!", true);
      break;
    case TEST_AUDIO_TONE:
      if (!s_toneActive) {
        s_tone.start();
        s_toneActive = true;
      }
      break;
    case TEST_CAMERA:
      if (s_cameraReady) {
        if (s_camera.captureDiagnosticToSd()) {
          s_photoCount++;
          char buf[32];
          snprintf(buf, sizeof(buf), "Photo #%lu saved!", (unsigned long)s_photoCount);
          setResult(buf, true);
        } else {
          strlcpy(s_instr, "Capture failed", sizeof(s_instr));
          s_needRedraw = true; hal::display::markAllDirty();
        }
      }
      break;
    case TEST_IMU:
      if (s_stepComplete) stepAdvance();
      break;
    case TEST_RECORDING:
      if (!s_isRecording) {
        s_toneActive = false;
        char path[48];
        snprintf(path, sizeof(path), "/recordings/REC%04lu.wav", (unsigned long)(s_recFileIdx + 1));
        const auto result = s_recording.begin(path);
        using Result = services::diagnostics::RawRecording::StartResult;
        if (result == Result::MicUnavailable) {
          strlcpy(s_instr, "Mic init failed!", sizeof(s_instr));
          s_needRedraw = true; hal::display::markAllDirty(); break;
        }
        ++s_recFileIdx;  // As before, consume the name only after mic initialization.
        if (result == Result::FileUnavailable) {
          strlcpy(s_instr, "File open failed!", sizeof(s_instr));
          s_needRedraw = true; hal::display::markAllDirty(); break;
        }
        s_isRecording = true;
        s_hasRecStatus = true;
        strlcpy(s_recStatus, "RECORDING", sizeof(s_recStatus));
        s_needRedraw = true; hal::display::markAllDirty();
      }
      break;
    default: break;
  }
}

static void onButtonARelease() {
  switch (s_step) {
    case TEST_VIBRATION:
      hal::io::setVibration(false);
      s_needRedraw = true; hal::display::markAllDirty();
      break;
    case TEST_AUDIO_TONE:
      s_toneActive = false;
      s_tone.stop();
      s_needRedraw = true; hal::display::markAllDirty();
      break;
    case TEST_RECORDING:
      if (s_isRecording) {
        s_isRecording = false;
        (void)s_recording.finish();
        s_recDuration = (float)s_recording.samples() / 48000.0f;
        char buf[48];
        snprintf(buf, sizeof(buf), "Saved! %.1fs  %lu smp", s_recDuration, (unsigned long)s_recording.samples());
        setResult(buf, true);
      }
      break;
    default: break;
  }
}

static void onButtonBPress() {
  if (s_step == TEST_BUTTON_B) {
    setResult("Button B works!", true);
    delay(300);
    stepAdvance();
    return;
  }
  if (s_step == TEST_COMPLETE || s_step == TEST_DISPLAY) { NavHandler::instance().pop(); return; }
  stepRetreat();
}

void onInput(hal::input::InputEvent ev) {
  using ET = hal::input::EventType;
  if (ev.type == ET::ButtonA_Press)   { s_buttonAHeld = true;  onButtonAPress();   return; }
  if (ev.type == ET::ButtonA_Release) { s_buttonAHeld = false; onButtonARelease(); return; }
  if (ev.type == ET::ButtonB_Press)   { onButtonBPress(); return; }
}

// ---- Lifecycle --------------------------------------------------------------

void enter() {
  s_step = 0; s_stepComplete = false; s_buttonAHeld = false;
  s_toneActive = false; s_cameraReady = false; s_photoCount = 0;
  s_isRecording = false; s_colorIdx = 0;
  clearDynamic();
  updateInstructions();
  s_needRedraw = true;
  hal::display::markAllDirty();
}

void exitApp() {
  hal::io::setVibration(false);
  s_toneActive = false;
  s_isRecording = false;
  s_recording.restoreUi();
  s_camera.stop();
  s_cameraReady = false;
}

#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool selectDiagnosticStep(const char* name, uint32_t recordingIndex) {
  if (recordingIndex && strcmp(name, "recording") != 0) return false;
  if (NavHandler::instance().top() != &app || s_step != TEST_DISPLAY ||
      s_buttonAHeld || s_toneActive || s_cameraReady || s_isRecording ||
      s_camera.running() || s_recording.active()) return false;
  int selected;
  if (strcmp(name, "audio") == 0) selected = TEST_AUDIO_TONE;
  else if (strcmp(name, "camera") == 0) selected = TEST_CAMERA;
  else if (strcmp(name, "recording") == 0) {
    const uint32_t next = recordingIndex ? recordingIndex : s_recFileIdx + 1;
    char path[48];
    snprintf(path, sizeof(path), "/recordings/REC%04lu.wav", (unsigned long)next);
    if (!hal::sd::mounted() || hal::sd::exists(path)) return false;
    if (recordingIndex) s_recFileIdx = recordingIndex - 1;
    selected = TEST_RECORDING;
  } else return false;
  s_step = selected;
  s_stepComplete = false;
  s_lastUpdateMs = 0;
  clearDynamic();
  updateInstructions();
  s_needRedraw = true;
  hal::display::markAllDirty();
  return true;
}

void dumpDiagnosticStatus(Print& output) {
  char path[48];
  snprintf(path, sizeof(path), "/recordings/REC%04lu.wav", (unsigned long)(s_recFileIdx + 1));
  output.printf("[hardware-test] active=%d step=%d held=%d tone=%d tx=%d camera=%d recording=%d samples=%lu next_record=%s exists=%d camera_session=%d\n",
      NavHandler::instance().top() == &app, s_step, s_buttonAHeld, s_toneActive,
      hal::audio::txReady(), s_cameraReady, s_isRecording,
      (unsigned long)s_recording.samples(), path, hal::sd::exists(path), s_camera.running());
}
#endif

static void screenTick(uint32_t) {
  tick();
}

static void enterView() { hal::display::markAllDirty(); }

static const ::scene::Screen kScreenHardwareTest = {
    "Hardware Test",
    enterView,
    nullptr,
    screenTick,
    onInput,
    nullptr,
    onDraw,
    "test",
    buildHardwareScene,
    &screen_chrome_layout::kHardwareTestTopBarColor,
};

static const ::scene::Screen* const kScreens[] = {&kScreenHardwareTest};
// Preserve the existing workflow reset on uncover/alarm return.
App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Hardware Test", "hardware", &kScreenHardwareTest,
                            enter, enter, exitApp,
                            "src/apps/hardware_test_app.cpp", kScreens, nullptr, nullptr);

}  // namespace hardware_test
}  // namespace apps
