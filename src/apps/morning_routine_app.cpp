#include "ui/default_font_metrics.h"
#include "config/screen_chrome_layout.h"
#include "apps/morning_routine_app.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "services/alarms.h"
#include "services/uploads.h"
#include "config/ui_layout.h"
#include "config/ui_theme.h"
#include "services/automation.h"
#include "services/media_delete.h"
#include "core/morning_quote_record.h"
#include "services/wav_capture.h"
#include "hal/hal_display.h"
#include "hal/hal_input.h"
#include "hal/hal_sd.h"
#include "hal/hal_wifi.h"
#include "nav_handler.h"
#include "scene/screen.h"
#include "services/morning_quote.h"
#include "ui/scene.h"
#include "ui/morning_style.h"
#include "ui/recording_wave.h"
#include "ui/text_wrap.h"
#include "ui/reflection_layout.h"
#include "ui/pixel_dialogue_frame.h"
#include "assets/kami_dialogue.h"
#include "ui/kami_dialogue.h"

namespace apps {
namespace morning_routine {
namespace {

using Record = core::morning_quote_record::Record;
using EventType = hal::input::EventType;

namespace style = ui::morning_style;
using Icon = style::Icon;
constexpr uint16_t kMorningBg = style::kBg;
constexpr uint16_t kMorningInk = style::kInk;
constexpr uint16_t kMorningPanel = 0xF6D5;
constexpr uint32_t kMaxReflectionMs = 2u * 60u * 1000u;

extern const ::scene::Screen kScreenQuote;
extern const ::scene::Screen kScreenPrompt;
extern const ::scene::Screen kScreenRecording;
extern const ::scene::Screen kScreenResult;
extern const ::scene::Screen kScreenError;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
extern const ::scene::Screen kScreenTestLauncher;
#endif

bool s_active = false;
bool s_needRedraw = false;
bool s_quoteAvailable = false;
bool s_quoteRetrying = false;
services::morning_quote::Status s_lastQuoteStatus = services::morning_quote::Status::Idle;
Record s_quote;
services::audio::WavCapture s_capture;

char s_recordingPath[96] = {};
char s_resultTitle[40] = {};
char s_resultDetail[96] = {};
char s_errorTitle[40] = {};
char s_errorDetail[96] = {};
uint32_t s_recordingStartedMs = 0;

uint32_t s_waveRevision = 0;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool s_devQueueFailure = false;
bool s_devQuote = false;
bool s_testStarted = false;
bool s_testQuoteReady = false;
#endif

void markRedraw() {
  s_needRedraw = true;
  hal::display::markAllDirty();
}

void prepareQuote() {
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  s_devQuote=false;
#endif
  s_active = true;
  // Catch up after a powered-off night, even when the next alarm is tomorrow.
  services::morning_quote::requestToday();
  s_quoteAvailable = services::morning_quote::getForToday(&s_quote);
  s_lastQuoteStatus=services::morning_quote::status();
  s_quoteRetrying=!s_quoteAvailable &&
      (s_lastQuoteStatus==services::morning_quote::Status::Fetching ||
       s_lastQuoteStatus==services::morning_quote::Status::Idle);
}

void resumeWorkflow() {
  if (app.currentScreen == &kScreenQuote) prepareQuote();
}

void switchScreen(const ::scene::Screen* screen) {
  if (screen == &kScreenQuote) prepareQuote();
  ::scene::switchTo(&app, screen);
  NavHandler::instance().invalidateChrome();
  markRedraw();
}

bool forward(const hal::input::InputEvent& event) {
  return event.type == EventType::ButtonB_Press || event.type == EventType::ButtonC_Press;
}

bool back(const hal::input::InputEvent& event) {
  return event.type == EventType::ButtonA_Press;
}

void finishRoutine() {
  NavHandler::instance().popToRoot();
}

bool allowChordExit() {
  return false;
}

void addPageBackground(ui::scene::Scene& scene, int16_t width, int16_t height) {
  scene.clear();
  scene.addRect("morning-routine.bg", {0, kSystemStatusBarH, width,
                (int16_t)(height - kSystemStatusBarH)}, kMorningBg);
}

void buildQuoteScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  addPageBackground(scene,width,height);
  if(s_quoteAvailable) {
    char quote[228];snprintf(quote,sizeof(quote),"\"%s\"",s_quote.quote);
    ui::kami_message::message(scene,width,height,"quote.line",quote);
    style::footer(scene,width,height,"B: Next",Icon::Next);
  } else {
    char message[256];
    snprintf(message,sizeof(message),"%s\n%s",s_quoteRetrying?"I'm finding today's quote.":"I couldn't get today's quote.",
        s_quoteRetrying?"This should only take a moment.":services::morning_quote::statusText());
    ui::kami_message::message(scene,width,height,"quote.detail",message);
    style::footer(scene,width,height,"B: Next",Icon::Next,s_quoteRetrying?nullptr:"A: Retry",Icon::Retry);
  }
}

void buildPromptScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  addPageBackground(scene,width,height);
  ui::kami_message::message(scene,width,height,"prompt.title","What's on your mind today?");
  style::footer(scene,width,height,"B: Talk",Icon::Mic,"A: Skip",Icon::Skip);
}

void buildRecordingScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  addPageBackground(scene, width, height);
  const uint32_t elapsed = millis() - s_recordingStartedMs;
  char timer[12];
  snprintf(timer, sizeof(timer), "%02lu:%02lu", (unsigned long)(elapsed / 60000u),
           (unsigned long)((elapsed / 1000u) % 60u));
  scene.addLabel("record.timer", {0, 49, width, 42}, timer, kMorningInk,
                 kMorningBg, 4, ui::scene::TextAlign::MiddleCenter, 2);
  ui::recordingWave(scene,width,s_capture.waveform().data());
  style::footer(scene,width,height,"B: Save",Icon::Save,"A: Cancel",Icon::Cancel);
}

void buildResultScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  addPageBackground(scene, width, height);
  ui::kami_message::message(scene,width,height,"result.title",s_resultTitle[0]?s_resultTitle:"Your reflection is saved.");
  style::footer(scene,width,height,"B: Home",Icon::Home);
}

void buildErrorScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  addPageBackground(scene, width, height);
  char message[320];snprintf(message,sizeof(message),"%s\n%s",s_errorTitle[0]?s_errorTitle:"I couldn't save your reflection.",s_errorDetail);
  ui::kami_message::message(scene,width,height,"error.detail",message);
  style::footer(scene,width,height,"B: Home",Icon::Home);
}

#ifdef ORIGAMI_DEV_SERIAL_INPUT
void buildTestLauncherScene(ui::scene::Scene& scene, int16_t width, int16_t height) {
  addPageBackground(scene, width, height);
  const char* message=s_testStarted?
      (s_testQuoteReady?"I'm starting your morning test. Today's cached quote is ready.":"I'm starting your morning test. The quote failure screen will be tested."):
      "I can't start the test while another alarm or recording is active.";
  ui::kami_message::message(scene,width,height,"test.message",message);
  if(!s_testStarted)style::footer(scene,width,height,"B: Retry",Icon::Retry,"A: Back",Icon::Cancel);
}
#endif

void drawScene(TFT_eSprite& canvas,
               void (*builder)(ui::scene::Scene&, int16_t, int16_t)) {
  if (!s_needRedraw) return;
  s_needRedraw = false;
  auto& scene = ui::scene::scratchScene();
  builder(scene, canvas.width(), canvas.height());
  scene.render(canvas);
  // Scaled expression frames may include white padding above the content band.
  NavHandler::instance().invalidateChrome();
  hal::display::markAllDirty();
}

void tickQuote(uint32_t now) {
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  if(s_devQuote) return;
#endif
  if (!s_quoteAvailable && services::morning_quote::getForToday(&s_quote)) {
    s_quoteAvailable = true;
    s_quoteRetrying = false;
    markRedraw();
    return;
  }
  const auto status=services::morning_quote::status();
  if (status!=s_lastQuoteStatus) {
    s_lastQuoteStatus=status;
    s_quoteRetrying=!s_quoteAvailable &&
        (status==services::morning_quote::Status::Fetching ||
         status==services::morning_quote::Status::Idle);
    markRedraw();
  }
}

void inputQuote(hal::input::InputEvent event) {
  if (forward(event)) {
    switchScreen(&kScreenPrompt);
  } else if (back(event) && !s_quoteAvailable && !s_quoteRetrying) {
    services::morning_quote::requestToday(true);
    s_quoteRetrying = true;
    markRedraw();
  }
}

void setError(const char* title, const char* detail) {
  strlcpy(s_errorTitle, title ? title : "Could not record", sizeof(s_errorTitle));
  strlcpy(s_errorDetail, detail ? detail : "Please try again.", sizeof(s_errorDetail));
  switchScreen(&kScreenError);
}

bool stopCapture() {
  return s_capture.finish();
}

void discardRecording() {
  stopCapture();
  if (s_recordingPath[0]) services::media_delete::deleteFile(s_recordingPath);
  s_recordingPath[0] = '\0';
  s_capture.release();
}

bool startRecording() {
  if (services::morning_quote::fetching()) {
    setError("Please wait", "Today's quote is still being prepared.");
    return false;
  }
  if (!hal::sd::mounted()) {
    setError("SD unavailable", "A card is required to save your reflection.");
    return false;
  }
  const auto result = s_capture.begin("/morning/reflections", "",
      s_recordingPath, sizeof(s_recordingPath),
      services::audio::WavCapture::CodecCheck::Required, true);
  if (result != services::audio::WavCapture::StartResult::Started) {
    s_capture.release();
    if (result == services::audio::WavCapture::StartResult::PeripheralBusy)
      setError("System busy", "Could not reserve audio and storage.");
    else if (result == services::audio::WavCapture::StartResult::AgentBusy)
      setError("System busy", "Could not pause the background connection.");
    else if (result == services::audio::WavCapture::StartResult::FileUnavailable)
      setError("SD write failed", "Could not create the reflection file.");
    else setError("Mic unavailable", "Check the microphone and try again.");
    return false;
  }
  s_recordingStartedMs = millis();
  s_capture.resetWaveform();
  s_waveRevision = 0;
  switchScreen(&kScreenRecording);
  return true;
}

void inputPrompt(hal::input::InputEvent event) {
  if (back(event)) finishRoutine();
  else if (forward(event)) (void)startRecording();
}

void saveReflection();

void tickRecording(uint32_t now) {
  if (!s_capture.isOpen()) return;
  if ((int32_t)(now - s_recordingStartedMs) >= (int32_t)kMaxReflectionMs) {
    saveReflection();
    return;
  }

  if (!s_capture.drain()) {
    stopCapture();
    if (s_recordingPath[0]) services::media_delete::deleteFile(s_recordingPath);
    s_recordingPath[0] = '\0';
    s_capture.release();
    setError("SD write failed", "Your reflection was not saved.");
    return;
  }
  if (s_waveRevision != s_capture.waveform().revision()) {
    s_waveRevision = s_capture.waveform().revision();
    markRedraw();
  }
}

void saveReflection() {
  const bool finalized=stopCapture();
  if(!finalized) {
    // Preserve the take for recovery, but never enqueue a damaged WAV or claim
    // success when the final buffered write/header update failed.
    Serial.printf("[morning-reflection] finalize failed path=%s\n",s_recordingPath);
    s_recordingPath[0]='\0';
    s_capture.release();
    setError("SD write failed","Could not finish the audio file.");
    return;
  }
  if (s_capture.samplesWritten() == 0) {
    if (s_recordingPath[0]) services::media_delete::deleteFile(s_recordingPath);
    s_recordingPath[0] = '\0';
    s_capture.release();
    setError("Nothing recorded", "Speak for a moment, then press Save.");
    return;
  }
  const bool hasPath = s_recordingPath[0] != '\0';
  bool allowQueue=true;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  allowQueue=!s_devQueueFailure;
  s_devQueueFailure=false;
#endif
  const bool queued = allowQueue && hasPath && services::uploads::queue().enqueue(
      "audio", s_recordingPath, "audio/wav", false);
  Serial.printf("[morning-reflection] saved=%u queued=%u samples=%lu path=%s\n",
      hasPath,queued,(unsigned long)s_capture.samplesWritten(),s_recordingPath);
  s_capture.release();
  if (!queued) {
    s_recordingPath[0] = '\0';
    setError("Saved only on SD", "It could not be queued for transcription.");
    return;
  }
  strlcpy(s_resultTitle, "Reflection saved", sizeof(s_resultTitle));
  strlcpy(s_resultDetail, hal::wifi::connected()
      ? "Sending for transcription in the background."
      : "It will be transcribed when WiFi returns.", sizeof(s_resultDetail));
  s_recordingPath[0] = '\0';
  switchScreen(&kScreenResult);
}

void inputRecording(hal::input::InputEvent event) {
  if (back(event)) {
    discardRecording();
    switchScreen(&kScreenPrompt);
  } else if (forward(event)) {
    saveReflection();
  }
}

void inputResult(hal::input::InputEvent event) {
  if (forward(event)) finishRoutine();
}

void inputError(hal::input::InputEvent event) {
  if (forward(event)) finishRoutine();
}

void drawQuote(TFT_eSprite& canvas) { drawScene(canvas, buildQuoteScene); }
void drawPrompt(TFT_eSprite& canvas) { drawScene(canvas, buildPromptScene); }
void drawRecording(TFT_eSprite& canvas) { drawScene(canvas, buildRecordingScene); }
void drawResult(TFT_eSprite& canvas) { drawScene(canvas, buildResultScene); }
void drawError(TFT_eSprite& canvas) { drawScene(canvas, buildErrorScene); }

#ifdef ORIGAMI_DEV_SERIAL_INPUT
void startCompleteTest() {
  Record quote{};
  // Exercise real quote availability. Automatically seeding the same fixture
  // every day hid backend failures and looked like a daily quote never changed.
  // Explicit `morning quote seed` remains available for layout-only bench work.
  s_testQuoteReady = services::morning_quote::getForToday(&quote);
  s_testStarted = services::automation::previewMorningAlarm(
      services::alarms::loadMorning().sound);
  Serial.printf("[morning-test] started=%u quote_ready=%u\n",
                s_testStarted, s_testQuoteReady);
  markRedraw();
}

void prepareTestLauncher() {
  // App resume follows alert dismissal and uncovering. Keep successful
  // launches one-shot until this app is popped; rejected launches may retry.
  if (s_testStarted) return;
  s_testQuoteReady = false;
  startCompleteTest();
}

void enterTestLauncher() { markRedraw(); }

void exitTestLauncher() {
  s_testStarted = false;
  s_testQuoteReady = false;
}

void inputTestLauncher(hal::input::InputEvent event) {
  if (back(event)) NavHandler::instance().pop();
  else if (forward(event) && !s_testStarted) startCompleteTest();
}

void drawTestLauncher(TFT_eSprite& canvas) {
  drawScene(canvas, buildTestLauncherScene);
}
#endif

void exitApp() {
  if (s_capture.isOpen() || s_recordingPath[0]) discardRecording();
  s_capture.release();
  s_capture.close();
  s_active = false;
  services::alarms::prepareNextMorningQuote();
}

const ::scene::Screen kScreenQuote = {
    "Quote", markRedraw, nullptr, tickQuote, inputQuote, allowChordExit,
    drawQuote, "quote", buildQuoteScene,
    &screen_chrome_layout::kMorningRoutineQuoteTopBarColor,
};
const ::scene::Screen kScreenPrompt = {
    "Reflection", markRedraw, nullptr, nullptr, inputPrompt, allowChordExit,
    drawPrompt, "reflection", buildPromptScene,
    &screen_chrome_layout::kMorningRoutineReflectionTopBarColor,
};
const ::scene::Screen kScreenRecording = {
    "Recording", markRedraw, nullptr, tickRecording, inputRecording,
    []() -> bool { return true; }, drawRecording, "recording", buildRecordingScene,
    &screen_chrome_layout::kMorningRoutineRecordingTopBarColor,
};
const ::scene::Screen kScreenResult = {
    "Saved", markRedraw, nullptr, nullptr, inputResult, allowChordExit,
    drawResult, "saved", buildResultScene,
    &screen_chrome_layout::kMorningRoutineSavedTopBarColor,
};
const ::scene::Screen kScreenError = {
    "Error", markRedraw, nullptr, nullptr, inputError, allowChordExit,
    drawError, "error", buildErrorScene,
    &screen_chrome_layout::kMorningRoutineErrorTopBarColor,
};
#ifdef ORIGAMI_DEV_SERIAL_INPUT
const ::scene::Screen kScreenTestLauncher = {
    "Launcher", enterTestLauncher, nullptr, nullptr, inputTestLauncher,
    allowChordExit, drawTestLauncher, "test-launcher", buildTestLauncherScene};
#endif

const ::scene::Screen* const kScreens[] = {
    &kScreenQuote, &kScreenPrompt, &kScreenRecording, &kScreenResult, &kScreenError};

}  // namespace

bool isActive() { return s_active; }
bool isRecording() { return s_active && s_capture.isOpen(); }

#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool developmentUi(const char* command) {
  if(strcmp(command,"cache-miss")==0 && !isRecording())
    return services::morning_quote::missCacheForDevelopment();
  if(strcmp(command,"fixture-restore")==0 && !isRecording())
    return services::morning_quote::restoreCacheFileForDevelopment();
  if(strcmp(command,"fixture-begin")==0 && !isRecording())
    return services::morning_quote::fixtureCacheForDevelopment(false);
  if(strcmp(command,"fixture-end")==0 && !isRecording())
    return services::morning_quote::fixtureCacheForDevelopment(true);
  if(strcmp(command,"cache-reload")==0 && !isRecording() && !services::morning_quote::fetching()) {
    services::morning_quote::begin(); return true;
  }
  if(strcmp(command,"status")==0) {
    Serial.printf("[morning-ui] screen=%s recording=%u samples=%lu level=%u wave=",
        app.currentScreen?app.currentScreen->name:"none",isRecording(),
        (unsigned long)s_capture.samplesWritten(),s_capture.waveform().level());
    for(int i=0;i<ui::kRecordingWaveBars;++i) Serial.printf("%u,",s_capture.waveform().data()[i]);
    Serial.println();
    return true;
  }
  if(strcmp(command,"queue-failure")==0) {
    s_devQueueFailure=true;
    Serial.println("[morning-ui] next enqueue will fail (development test)");
    return true;
  }
  if(!isActive() || isRecording()) return false;
  if(strcmp(command,"quote-unavailable")==0 || strcmp(command,"quote-loading")==0) {
    switchScreen(&kScreenQuote);
    s_quoteAvailable=false; s_devQuote=true;
    s_quoteRetrying=strcmp(command,"quote-loading")==0;
    markRedraw();
    return true;
  }
  if(strncmp(command,"quote ",6)==0 && strlen(command+6)<sizeof(s_quote.quote)) {
    switchScreen(&kScreenQuote);
    strlcpy(s_quote.quote,command+6,sizeof(s_quote.quote));
    s_quoteAvailable=true; s_devQuote=true;
    markRedraw();
    return true;
  }
  if(strcmp(command,"error")==0) {
    setError("SD unavailable","A card is required to save your reflection.");
    return true;
  }
  return false;
}
#endif

static core::health::RuntimePolicy runtimePolicy() {
    return core::health::whileBusy(isActive());
}
App app = SCENE_APP_CATALOG_LIFECYCLE_HOOKS("Morning Routine", "morning-routine", &kScreenQuote,
                            prepareQuote, resumeWorkflow, exitApp, "src/apps/morning_routine_app.cpp", kScreens, nullptr, runtimePolicy);

#ifdef ORIGAMI_DEV_SERIAL_INPUT
App testApp = {"Test Morning Routine", &kScreenTestLauncher, nullptr, nullptr,
               exitTestLauncher, nullptr, nullptr, nullptr, 0, nullptr, runtimePolicy,
               prepareTestLauncher, prepareTestLauncher};
#endif

}  // namespace morning_routine
}  // namespace apps
