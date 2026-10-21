#include "config/screen_chrome_layout.h"
/**
 * @file voice_recorder_app.cpp
 * @brief Voice recorder — continuous WAV capture with live mic level visualizer.
 *
 * Audio pipeline
 * --------------
 *   services::audio::WavCapture owns I²S RX, 48 kHz conversion/tuning,
 *   waveform metering and WAV writing. The app owns recording workflow,
 *   naming, wall-clock timing and the save/upload decision.
 *
 * State machine
 * -------------
 *   IDLE       — B starts a new recording; A+B leaves the app.
 *   B pauses/resumes capture; A stops, saves and resets; C is unused.
 *   RECORDING  — mic active, WAV file open, samples flowing to SD.
 *   PAUSED     — WAV file remains open, but samples/timer are paused.
 *
 * Display bypass
 * --------------
 * Like the MP3 now-playing screen, this app uses direct-TFT rendering
 * (enterOptimizedRender) to avoid SPI bus contention between the canvas flush and
 * the SD WAV writes.  All draw calls go through vrTft() which resolves to
 * hal::display::directTft() when bypass is active.
 *
 * Display updates
 * ---------------
 * The mockup and device share a pixel renderer. Small sprite bands update the
 * waveform, character and timer without repainting the entire screen.
 *
 * Timing
 * ------
 * Elapsed time is tracked via millis() wall-clock segments (s_accumMs + running
 * delta) rather than sample counting so the displayed MM:SS is correct even if the
 * codec sample rate drifts slightly or a DMA under-run occurs.
 *
 * Mic level visualizer
 * --------------------
 * The shared RecordingWaveform measures raw 48 kHz input in 100ms windows,
 * before recording-specific DSP, exactly as morning reflection and the agent do.
 */

#include "ui/coverage_surface.h"
#include "ui/direct_bands.h"
#include "apps/voice_recorder_app.h"
#include "services/uploads.h"

#include <Arduino.h>
#include "app.h"
#include "hal/hal_input.h"
#include "core/recording_files.h"
#include "services/wav_capture.h"
#include <cstring>

#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "hal/hal_bus.h"
#include "hal/hal_display.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "ui/draw.h"
#include "ui/playback_status.h"
#include "ui/recording_wave.h"
#include "ui/morning_style.h"
#include "ui/scene.h"
#include "ui/source_layout.h"
#include "ui/recorder_view.h"

namespace apps {
namespace voice_recorder {

// Same display backdoor as MP3 now-playing: canvas flush() contends with SD writes
// on the shared SPI bus. Draw direct to the panel for the lifetime of this app.
static bool s_displayBypass = false;
static bool s_appActive = false;

extern const ::scene::Screen kScreenRecorderReady;
extern const ::scene::Screen kScreenRecorderSaved;
static uint32_t s_savedAt=0;
extern const ::scene::Screen kScreenRecorderRecording;

static inline TFT_eSPI& vrTft() {
  return s_displayBypass ? hal::display::directTft() : hal::display::tft();
}

static void setDisplayBypass(bool on) {
  if (on == s_displayBypass) return;
  s_displayBypass = on;
  if (on) hal::display::enterOptimizedRender();
  else { hal::display::exitOptimizedRender(); NavHandler::instance().invalidateChrome(); }
}

enum class RecState { IDLE, RECORDING, PAUSED };

static services::audio::WavCapture s_capture;

static RecState s_state     = RecState::IDLE;
static uint32_t s_waveRevision = 0;
static uint32_t s_lastAnimationMs = 0;
static uint32_t s_ignoreRbUntilMs = 0;

// Wall-clock based timing — independent of mic sample-rate quirks so the
// displayed MM:SS reflects real elapsed seconds even if the codec drifts.
static uint32_t s_segmentStartMs = 0;  // when current RECORDING segment started
static uint32_t s_accumMs        = 0;  // total recorded ms in prior segments

static char s_wave[47] = {};
static unsigned s_elapsedSecond = UINT32_MAX;

static constexpr uint32_t kMaxRecordingMs = 4u * 60u * 1000u;

static char s_currentRecordingPath[96] = {0};
static const char* s_recordingError = nullptr;

// --- WAV plumbing ----------------------------------------------------------

static void enqueueCurrentRecording() {
  if (!s_currentRecordingPath[0]) return;
  if (!core::recording_files::hasAudio(s_currentRecordingPath)) {
    Serial.printf("[recorder] Not queueing empty WAV: %s\n", s_currentRecordingPath);
    s_currentRecordingPath[0] = '\0';
    return;
  }

  const bool queued = services::uploads::queue().enqueue(
      "audio", s_currentRecordingPath, "audio/wav", false);
  Serial.printf("[recorder] %s audio upload: %s\n",
                queued ? "Queued" : "Failed to queue",
                s_currentRecordingPath);
  s_currentRecordingPath[0] = '\0';
}

// --- Timing ----------------------------------------------------------------

static uint32_t elapsedMs() {
  if (s_state == RecState::RECORDING) {
    return s_accumMs + (uint32_t)(millis() - s_segmentStartMs);
  }
  return s_accumMs;
}

static void timingResetForFreshRecording() {
  s_accumMs        = 0;
  s_segmentStartMs = millis();
}

static void timingPause() {
  if (s_state == RecState::RECORDING) {
    s_accumMs += (uint32_t)(millis() - s_segmentStartMs);
  }
}

static void timingResume() {
  s_segmentStartMs = millis();
}

static bool closeCaptureHardware() {
  if (s_state == RecState::RECORDING) timingPause();
  return s_capture.finish(services::audio::WavCapture::FinishMode::ReleaseMicrophone);
}

static bool saveCurrentRecording() {
  if (closeCaptureHardware()) {
    s_recordingError = core::recording_files::hasAudio(s_currentRecordingPath) ? nullptr : "No audio saved";
    enqueueCurrentRecording();
  } else {
    Serial.printf("[recorder] WAV finalization failed; file retained, not queued: %s\n",
                  s_currentRecordingPath);
    s_recordingError = "Save failed";
    s_currentRecordingPath[0] = '\0';
  }
  s_capture.resetWaveform();
  s_waveRevision = 0;
  s_wave[0] = 0;
  s_accumMs = 0;
  s_state = RecState::IDLE;
  s_ignoreRbUntilMs = millis() + 350u;
  s_capture.release();
  return s_recordingError == nullptr;
}

// Shared pixel renderer: the preview and panel have identical geometry and fonts.
static int viewState() {
  return s_state==RecState::RECORDING?1:s_state==RecState::PAUSED?2:0;
}
static void drawRegion(int x,int y,int w,int h) {
  const unsigned elapsed=elapsedMs()/1000;
  const unsigned phase=elapsedMs();
  ui::drawDirectBands(x,y,w,h,[&](ui::BandSurface& surface) {
    recorder_view::render(surface,viewState(),elapsed,phase,0,s_wave);
  });
}

static void buildVoiceScene(ui::scene::Scene& graph,int16_t,int16_t) {
  static uint8_t* pixels=nullptr;
  if(!pixels) pixels=(uint8_t*)ps_malloc(280*240/2);
  graph.clear();
  if(!pixels) return;
  ui::CoverageSurface surface{pixels,280,240};
  recorder_view::render(surface,viewState(),elapsedMs()/1000,elapsedMs(),0,s_wave);
  graph.addBitmap("recorder",{0,0,280,240},pixels,280,240,0x2104,0xFFFF,0,4);
}
static void drawRecordingError();
static void drawFullUi() {
  if (s_recordingError) { drawRecordingError(); return; }
  s_elapsedSecond=elapsedMs()/1000;
  drawRegion(0,0,280,240);
}
static void drawTimer(uint32_t totalMs) {
  const unsigned seconds=totalMs/1000;
  if(seconds==s_elapsedSecond) return;
  s_elapsedSecond=seconds;
  drawRegion(recorder_view::grid::text,recorder_view::grid::hero,168,40);
}

void enter() {
  s_appActive = true;
  s_recordingError = nullptr;
  setDisplayBypass(true);
  s_state = RecState::IDLE;
  s_capture.resetWaveform();
  s_waveRevision = 0;
  s_wave[0] = 0;
  s_accumMs = 0;
  s_currentRecordingPath[0] = '\0';
}

void exitApp() {
  if (s_state == RecState::RECORDING || s_capture.isOpen() || s_currentRecordingPath[0]) {
    saveCurrentRecording();
  }
  s_capture.close();
  setDisplayBypass(false);
  s_appActive = false;
  s_capture.release();
}

bool usesDirectPanel() { return s_displayBypass; }

bool isRecording() {
  return s_state == RecState::RECORDING && s_capture.isOpen();
}

#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool developmentUi(const char* command) {
  if (strcmp(command,"status")!=0) return false;
  Serial.printf("[recorder-ui] active=%d recording=%d samples=%lu level=%u wave=",
      s_appActive?1:0,isRecording()?1:0,(unsigned long)s_capture.samples(),s_capture.waveform().level());
  for (int i=0;i<ui::kRecordingWaveBars;++i) Serial.printf("%u,",s_capture.waveform().data()[i]);
  Serial.println();
  return true;
}
#endif

static bool startRecording();
static void pauseActiveRecording();
static void resumePausedRecording();

void tick() {
  if (s_recordingError) return;
  if (s_state == RecState::RECORDING && elapsedMs() >= kMaxRecordingMs) {
    Serial.println("[recorder] Four-minute recording limit reached; saving");
    const bool saved=saveCurrentRecording();
    ::scene::switchTo(&app, saved?&kScreenRecorderSaved:&kScreenRecorderReady, saved?::scene::ScreenTransition::Forward : ::scene::ScreenTransition::Back);
    return;
  }

  // Timer tracks wall clock — paint *before* SD/mic work so long writes cannot stall the clock.
  drawTimer(elapsedMs());
  if (s_state == RecState::RECORDING && !s_capture.drain()) {
    timingPause();
    s_capture.finish(services::audio::WavCapture::FinishMode::ReleaseMicrophone);
    s_currentRecordingPath[0] = '\0';
    s_state = RecState::IDLE;
    s_capture.release();
    ui::Painter es(vrTft());
    es.clear();
    es.emptyState("SD write failed");
    return;
  }

  if (s_waveRevision != s_capture.waveform().revision()) {
    const unsigned count=min((unsigned)(s_capture.waveform().revision()-s_waveRevision),27u);
    for(unsigned i=27-count;i<27;++i) {
      size_t n=strlen(s_wave);
      if(n==46) { memmove(s_wave,s_wave+1,45); n=45; }
      s_wave[n]='A'+(s_capture.waveform().data()[i]*25+254)/255;
      s_wave[n+1]=0;
    }
    s_waveRevision=s_capture.waveform().revision();
    if(s_state==RecState::RECORDING) {

      drawRegion(recorder_view::grid::left,recorder_view::grid::wave-32,232,64);
    }
  }
  if(s_state==RecState::RECORDING && millis()-s_lastAnimationMs>=80) {
    s_lastAnimationMs=millis();
    drawRegion(recorder_view::grid::left,84,48,32);
  }
}

static void drawRecordingError() {
  ui::Painter error(vrTft());
  error.clear();
  error.emptyState(s_recordingError);
}

static bool startRecording() {
  const auto result = s_capture.begin("/recordings", "REC_",
      s_currentRecordingPath, sizeof(s_currentRecordingPath),
      services::audio::WavCapture::CodecCheck::BestEffort);
  if (result != services::audio::WavCapture::StartResult::Started) {
    s_currentRecordingPath[0] = '\0';
    s_capture.release();
    s_recordingError = result == services::audio::WavCapture::StartResult::PeripheralBusy
        ? "Network busy" : result == services::audio::WavCapture::StartResult::FileUnavailable
        ? "SD open failed" : "Mic failed";
    drawRecordingError();
    return false;
  }
  s_recordingError = nullptr;
  s_capture.resetWaveform();
  s_waveRevision = 0;
  s_wave[0] = 0;
  timingResetForFreshRecording();
  s_state = RecState::RECORDING;
  ::scene::switchTo(&app, &kScreenRecorderRecording);
  drawFullUi();
  return true;
}

static void pauseActiveRecording() {
  if (s_state != RecState::RECORDING) return;
  timingPause();
  s_state = RecState::PAUSED;
  drawFullUi();
}

static void resumePausedRecording() {
  if (s_state != RecState::PAUSED) return;
  timingResume();
  s_state = RecState::RECORDING;
  drawFullUi();
}

void onInput(hal::input::InputEvent ev) {
  using ET = hal::input::EventType;
  if(ev.type==ET::ButtonA_Tap) {
    if(s_state==RecState::RECORDING || s_state==RecState::PAUSED) {
      const bool saved=saveCurrentRecording();
      ::scene::switchTo(&app,saved?&kScreenRecorderSaved:&kScreenRecorderReady, saved?::scene::ScreenTransition::Forward : ::scene::ScreenTransition::Back);
    }
  } else if(ev.type==ET::ButtonB_Tap) {
    if(s_state==RecState::IDLE) {
      if((int32_t)(millis()-s_ignoreRbUntilMs)>=0) (void)startRecording();
    } else if(s_state==RecState::RECORDING) pauseActiveRecording();
    else if(s_state==RecState::PAUSED) resumePausedRecording();
  }
  // C is unused. A+B follows the navigation chord and exitApp saves the take.
}

static void screenTick(uint32_t) {
  tick();
}

static void resumeWorkflow() {
  // Preserve recovery after a failed drain without making view entry start I/O.
  s_recordingError = nullptr;
  if (app.currentScreen == &kScreenRecorderRecording && s_state == RecState::IDLE)
    (void)startRecording();
}

static void enterRecorderView() {
  setDisplayBypass(true);
  if (s_recordingError) drawRecordingError();
  else drawFullUi();
}

static void buildSavedScene(ui::scene::Scene& graph,int16_t width,int16_t height) {
  graph.clear();graph.addRect("saved.bg",{0,0,width,height},kUiSuccessBg);
  ui::morning_style::statusMark(graph,width,ui::morning_style::StatusMark::Success,kUiSuccessBg);
  graph.addLabel("saved.title",{16,120,int16_t(width-32),32},"Saved",kUiFg,kUiSuccessBg,2,ui::scene::TextAlign::MiddleCenter,2);
}
static void enterSaved(){setDisplayBypass(false);s_savedAt=millis();hal::display::markAllDirty();}
static void drawSaved(TFT_eSprite& canvas){
  auto& graph=ui::scene::scratchScene();buildSavedScene(graph,canvas.width(),canvas.height());
  ui::scene::setCurrentScene(&graph);
  graph.render(canvas);hal::display::markAllDirty();
}
static void tickSaved(uint32_t now){if(int32_t(now-s_savedAt)>=2000)::scene::switchTo(&app,&kScreenRecorderReady,::scene::ScreenTransition::Back);}
static void inputSaved(hal::input::InputEvent ev){
  using ET=hal::input::EventType;
  if(millis()-s_savedAt>=350u && (ev.type==ET::ButtonA_Tap || ev.type==ET::ButtonB_Tap || ev.type==ET::ButtonC_Tap))
    ::scene::switchTo(&app,&kScreenRecorderReady,::scene::ScreenTransition::Back);
}
const ::scene::Screen kScreenRecorderSaved={"Saved",enterSaved,nullptr,tickSaved,inputSaved,nullptr,drawSaved,"saved",buildSavedScene};

const ::scene::Screen kScreenRecorderReady = {
    "Ready",
    enterRecorderView,
    nullptr,
    screenTick,
    onInput,
    nullptr,
    nullptr,
    "ready",
    buildVoiceScene,
    &screen_chrome_layout::kRecorderReadyTopBarColor,
};

const ::scene::Screen kScreenRecorderRecording = {
    "Recording",
    enterRecorderView,
    nullptr,
    screenTick,
    onInput,
    nullptr,
    nullptr,
    "recording",
    buildVoiceScene,
    &screen_chrome_layout::kRecorderRecordingTopBarColor,
};

static const ::scene::Screen* const kScreens[] = {
    &kScreenRecorderSaved,
    &kScreenRecorderRecording,
    &kScreenRecorderReady,
};
static AppPresentation presentationPolicy() {
    return {false, false, usesDirectPanel() ? AppPresentation::StatusBar::Hidden : AppPresentation::StatusBar::DirectWhenOptimized};
}
static core::health::RuntimePolicy runtimePolicy() {
    return core::health::whileBusy(isRecording());
}
App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Audio Recorder", "recorder", &kScreenRecorderReady, enter, resumeWorkflow, exitApp,
                            "src/apps/voice_recorder_app.cpp", kScreens, presentationPolicy, runtimePolicy);

}  // namespace voice_recorder
}  // namespace apps
