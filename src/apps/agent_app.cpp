#include "services/agent_voice.h"
#include "services/agent_transport.h"
// agent_app.cpp — Kami app coordinator.
//
// Coordinates app workflow and view entry around the NavHandler-managed
// lifecycle. Rendering and operational implementation are delegated:
//
//   • Per-screen rendering / event handling → src/apps/agent/screen_*.cpp
//   • Cross-screen flow (record, send, return-to-home) → agent_flow.cpp
//   • PSRAM diagnostics / sprite alloc → core::memory
//   • Persistent WebSocket PCM/reply streaming → services/agent_transport.cpp
//
// Tweakable knobs live in include/apps/agent/agent_config.h.
// Shared state lives in include/apps/agent/agent_state.h.
//
// External callers (main_menu.cpp and tooling) interact
// only via the functions declared in include/apps/agent_app.h.

#include "apps/agent_app.h"
#include "apps/menu_app.h"
#include "nav_handler.h"

#include <Arduino.h>

#include "app.h"
#include "apps/agent/agent_config.h"
#include "apps/agent/agent_flow.h"
#include "apps/agent/agent_emotion.h"
#include "apps/agent/agent_render.h"
#include "apps/agent/agent_screens.h"
#include "ui/kami_motion.h"
#include "core/memory.h"
#include "scene/screen.h"
#include "sfx/sfx_typewriter.h"
#include "ui/listening_style.h"
#include "apps/agent/agent_motion.h"
#include "apps/settings_menu_app.h"
#include "nav_handler.h"
#include "sfx/sfx.h"

namespace apps {
namespace agent {

// ─── Convenience accessor ────────────────────────────────────────────────────

static inline AgentState& S() { return flow::state(); }

#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool developmentUi(const char* command) {
    AgentState& s=S();
    if(strncmp(command,"shake ",6)==0){
        char* end=nullptr;long duration=strtol(command+6,&end,10);
        return end!=command+6 && !*end && duration>0 && testEyesShake(duration);
    }
    if(strcmp(command,"cancel")==0){
        flow::cancelDelegation();flow::returnToHome();NavHandler::instance().popToRoot();return true;
    }
    if(strncmp(command,"motion ",7)==0){
        char* end=nullptr;long age=strtol(command+7,&end,10);
        return end!=command+7 && !*end && motion::seek(age);
    }
    if(strcmp(command,"delegate-wait")==0) {
        flow::returnToHome();
        if(!s.conv.ensure() || !s.conv.pushMessage("I'm checking..."))return false;
        flow::awaitDelegatedReplies(1);
        flow::goTo(Stage::Reply);return true;
    }
    if(strcmp(command,"status")==0) {
        Serial.printf("[agent-delivery] waiting=%u\n",flow::waitingForDelegation()?1:0);
        motion::status();
        Serial.printf("[agent-style] id=%d name=%s ready=%d render_us=%lu\n",
            ui::listening_style::selected(),ui::listening_style::kStyles[ui::listening_style::selected()].name,
            ui::listening_style::ready(),(unsigned long)ui::listening_style::lastRenderUs());
        Serial.printf("[agent-ui] stage=%d segment=%d scroll=%d lines=%d lineHeight=%d typing=%d open=%d emotion=%s revision=%lu\n",
          (int)s.stage,s.msgIndex,s.scrollLines,s.agentMaxLines,s.agentLineH,
          render::typewriterInProgress(s)?1:0,s.miniEyesOpen?1:0,s.miniEmotionName,(unsigned long)s.miniEmotionRevision);
        const auto replies=services::agent_transport::replyQueueStats();
        Serial.printf("[agent-ui] reply queued=%lu peak=%lu events=%lu done=%u\n",
          (unsigned long)replies.bytes,(unsigned long)replies.peakBytes,
          (unsigned long)replies.events,services::agent_transport::status().done?1:0);
        Serial.printf("[agent-notice] kind=%u mic=%u slab=%u previous=%u armed=%u wifi=%u\n",(unsigned)s.noticeKind,services::agent_voice::isCapturing(),services::agent_voice::capacity()!=0,s.hasPreviousReply,s.noticeInput.armed,flow::wifiReady());
        if (s.stage == Stage::Recording) {
            Serial.printf("[agent-border] raw=%u thickness=%d override=%d\n",
                flow::micLevel(),s.recordingBorderThickness,s.recordingLevelOverride);
            Serial.printf("[agent-ui] listening=1 samples=%lu level=%u wave=",
                (unsigned long)s.pcmN,flow::micLevel());
            for (int i=0; i<core::audio::RecordingWaveform::kBars; ++i)
                Serial.printf("%u,",services::agent_voice::waveform().data()[i]);
            Serial.println();
        }
        return true;
    }
    if (strncmp(command,"style ",6)==0) {
        char* end=nullptr; const long index=strtol(command+6,&end,10);
        if(end==command+6 || *end) return false;
        return ui::listening_style::select(index,false);
    }
    if(strcmp(command,"styles")==0 && s.stage==Stage::Eyes) {
        apps::settings_menu::openAgentStyles();
        return true;
    }
    if (strncmp(command,"level ",6)==0 && s.stage==Stage::Recording) {
        char* end = nullptr;
        const long level = strtol(command+6,&end,10);
        if (end==command+6 || *end || level < -1 || level > 255) return false;
        s.recordingLevelOverride = int(level);
        return true;
    }
    if(!strncmp(command,"wifi ",5)){
        if(s.stage==Stage::Recording||s.stage==Stage::Thinking||flow::isSessionActive())return false;
        const char* v=command+5;
        if(strcmp(v,"auto")&&strcmp(v,"offline")&&strcmp(v,"online"))return false;
        s.wifiOverride=!strcmp(v,"auto")?-1:!strcmp(v,"online")?1:0;return true;
    }
    if(s.stage==Stage::Recording || s.stage==Stage::Thinking || flow::isSessionActive())return false;
    if(!strncmp(command,"notice ",7)){
        const char* v=command+7;
        const char* raw=!strcmp(v,"offline")?"WiFi not connected":!strcmp(v,"reconnecting")?"Reconnecting Wi-Fi":!strcmp(v,"mic")?"Mic init failed":!strcmp(v,"empty")?"No audio recorded":!strcmp(v,"timeout")?"Agent connection timed out":!strcmp(v,"service")?"WebSocket error":nullptr;
        if(!raw)return false;
        snprintf(s.errBuf,sizeof(s.errBuf),"%s",raw);
        if(s.stage==Stage::Error){s.noticeKind=notice::classify(raw,flow::wifiReady());s.noticeSince=millis();s.noticeInput.reset();render::paintError(s);}
        else flow::goTo(Stage::Error);
        return true;
    }

    if(!strncmp(command,"preview ",8)) {
        const char* step=command+8;
        if(strcmp(step,"listen") && strcmp(step,"followup") && strcmp(step,"think") && strcmp(step,"reply"))return false;
        if(s.stage!=Stage::Reply){
            flow::returnToHome();if(!s.conv.ensure())return false;s.conv.clear();
            s.conv.pushMessage("A quiet moment. What would you like to explore next?");
            flow::goTo(Stage::Reply);
        }
        if(!strcmp(step,"listen")||!strcmp(step,"followup")){
            motion::beforeStage(!strcmp(step,"followup")?Stage::Reply:Stage::Eyes,Stage::Recording);
            motion::startListening(millis());
        }else if(!strcmp(step,"think")){
            motion::beforeStage(Stage::Recording,Stage::Thinking);motion::startThinking();
        }else motion::startReply();
        return true;
    }
    if(!strncmp(command,"emotion ",8)&&s.stage==Stage::Reply) {
        if(!emotion::developmentSet(command+8))return false;
        s.msgIndex=0;s.miniEyesOpen=true;s.miniBlinkMs=millis()+10000;
        render::paintMiniEyes(s);return true;
    }
    const bool reply=strcmp(command,"reply")==0;
    const bool thinking=strcmp(command,"thinking")==0;
    const bool direct=strcmp(command,"direct")==0;
    if(direct && s.stage==Stage::Reply) {
        render::releaseChatSprites(s);render::paintReplyText(s);return true;
    }
    if(strcmp(command,"blink")==0 && s.stage==Stage::Reply) {
        s.miniEyesOpen=!s.miniEyesOpen;s.miniBlinkMs=millis()+10000;
        render::paintMiniEyes(s);return true;
    }
    if(!reply && !thinking)return false;
    flow::returnToHome();
    if(!s.conv.ensure())return false;
    s.conv.clear();
    if(reply) {
        s.conv.pushMessage("Start with one small step. Pick something you can finish today, and give yourself time to notice how it feels. Keep going gently.");
        s.conv.pushMessage("You have a lot on your mind. What feels most important today?");
    }
    flow::goTo(Stage::Reply);
    if(thinking) {
        // Exercise the live renderer without starting the microphone/network flow.
        strlcpy(s.thinkStatus,"Thinking",sizeof(s.thinkStatus));
        render::paintThinkingShell(s,2);
    }
    return true;
}
#endif

// ─── App lifecycle (NavHandler vtable) ───────────────────────────────────────

void prepareHome() {

    AgentState& s = S();
    s.cancelResult       = CancelTarget::None;
    s.chordHoldDeadlineMs = 0;
    s.prevStageForIdle   = Stage::Eyes;
    s.sendJustFinished   = false;
    s.animFrame          = 0;
    s.pcmN = 0;
    if (!flow::waitingForDelegation()) services::agent_voice::resetForEntry();

    // Do NOT acquire the slab here — slab lives alongside eye sprites on the
    // home screen and routinely exhausts contiguous PSRAM. Acquired on first
    // PTT instead (see flow::startRecording).
    render::releaseChatSprites(s);

    (void)flow::ensureTransport(true);

    // The agent is the root app; on first push we're always on Eyes.
    s.stage = Stage::Eyes;
    (void)ui::listening_style::prepare();
    NavHandler::instance().runtimePolicyChanged();
}

void enterEyesScreen() {
    if(S().resumeNotice){
        S().resumeNotice=false; S().stage=Stage::Eyes; flow::goTo(Stage::Error); return;
    }
    motion::prepareHome();
    prepareEyesView(S());
    motion::startHome();
    motion::tick(S(),millis());
    sfx::loadVolume();
    // Preserve the post-view retry if initial app preparation could not spawn
    // the persistent transport. View allocation and volume loading precede it.
    (void)flow::ensureTransport();
}

static void resumeWorkflow() {
    // Overlay dismissal retains its interrupted view; only an Eyes return
    // performs Home workflow setup. Uncover/root refresh select Eyes first.
    if (app.currentScreen == &kScreenEyes) prepareHome();
}

void exitApp() {
    stopEyesFeedback();
    flow::cancelDelegation();
    motion::cancel();
    ui::listening_style::releaseCache();
    // Teardown owns cancellation; callers need not know the Agent's stage.
    requestAbortSendingToEyes();
    services::agent_voice::releaseResources();

    AgentState& s = S();
    s.cancelResult       = CancelTarget::None;
    s.recStartedByChord  = false;
    s.chordHoldDeadlineMs = 0;

    if (s.stage == Stage::Recording) {
        services::agent_voice::finish();
        s.pcmN = services::agent_voice::samples();
        flow::cancelSend(CancelTarget::Eyes);
    }
    sfx::typewriterTeardown();
    services::agent_voice::restorePlayback();

    s.stage = Stage::Eyes;
    NavHandler::instance().runtimePolicyChanged();
    render::releaseChatSprites(s);
    services::agent_voice::releaseBuffers();
    s.conv.release();
    s.previousReply.release();s.hasPreviousReply=false;
}

void serviceScreenTick(uint32_t now) {
    AgentState& s = S();
    ui::kami_motion::poll(now,s.stage==Stage::Eyes,s.stage==Stage::Eyes);

    // Track stage transitions so the menu idle pulse fires after a finished
    // send/record (otherwise a long send would trigger battery sleep instantly).
    // The Thinking → * transition fires for all destinations (Reply, Error,
    // Eyes) since the user clearly just interacted regardless of outcome.
    if (s.prevStageForIdle == Stage::Thinking && s.stage != Stage::Thinking) {
        s.pulseMenuActivity = true;
    } else if (s.prevStageForIdle == Stage::Recording && s.stage != Stage::Recording &&
               !s.conv.held()) {
        s.pulseMenuActivity = true;
    }
    s.prevStageForIdle = s.stage;
}

void prepareScreenInput(hal::input::InputEvent ev) {
    AgentState& s = S();
    using ET = hal::input::EventType;

    // Chord-hold cancellation: if a button is released while the hold window
    // is armed and the other button is also up, cancel the upgrade-to-record.
    if (s.chordHoldDeadlineMs != 0 &&
        (ev.type == ET::ButtonA_Release || ev.type == ET::ButtonB_Release)) {
        if (!hal::input::buttonADown() || !hal::input::buttonBDown()) {
            s.chordHoldDeadlineMs = 0;
        }
    }
}

// ─── Public predicates ───────────────────────────────────────────────────────

bool isAtHomescreen()    { return S().stage == Stage::Eyes; }
bool isOnReplyScreen()   { return S().stage == Stage::Reply; }
bool isShowingError()    { return S().stage == Stage::Error; }
bool isRecording()       { return S().stage == Stage::Recording; }
void drawRecordingOverlay() {
    if (isRecording()) render::paintRecordingBorder(S(), millis());
}
uint8_t recordingLevel() { return flow::micLevel(); }
bool isSending()         { return S().stage == Stage::Thinking; }
bool isConversationTyping() { return render::typewriterInProgress(S()); }

// ─── App workflow service before the visible screen ticks ───────────────────

void serviceMicCaptureEarly() {
    AgentState& s = S();
    if (s.stage != Stage::Recording || !services::agent_voice::isCapturing()) return;
    const bool first = s.pcmN == 0;
    services::agent_voice::tick();
    s.pcmN = services::agent_voice::samples();
    if (first && s.pcmN) Serial.printf("[voice-entry] first_pcm_ms=%lu samples=%lu\n",
        (unsigned long)(millis()-s.voiceEntryMs), (unsigned long)s.pcmN);
}

static void preTick(uint32_t now) {
    serviceMicCaptureEarly();
    flow::pollDelegation(now);
    if(NavHandler::instance().top()!=&app) {
        flow::cancelDelegation();
        if(flow::isSessionActive()) services::agent_transport::abandon();
    }
}

void returnToHomescreen() { flow::returnToHome(); }
void requestExitToMenu() {
    NavHandler& nav = NavHandler::instance();
    if (nav.depth() <= 1) nav.push(&apps::menu::app);
    else nav.pop();
}

// ─── Send abort API (called from services / nav_handler) ─────────────────────

void requestAbortSendingToMenu() {
    if (S().stage != Stage::Thinking) return;
    flow::cancelSend(CancelTarget::Menu);
}

void requestAbortSendingToEyes() {
    if (S().stage != Stage::Thinking) return;
    flow::cancelSend(CancelTarget::Eyes);
}

// ─── Reply screen accessors (legacy public API) ──────────────────────────────

// Forward declarations of helpers defined in screen_reply.cpp.
bool replyScrollByLines(int delta);
bool replyAdvancePage();

bool scrollReplyWithRotary(int lineDelta) {
    if (S().stage != Stage::Reply) return false;
    return replyScrollByLines(lineDelta);
}

bool advanceReplyPage() {
    if (S().stage != Stage::Reply) return false;
    return replyAdvancePage();
}

void scrollConversationDown() {
    if (S().stage != Stage::Reply) return;
    replyScrollByLines(1);
}

// ─── One-shot pulse consumers ────────────────────────────────────────────────

bool takeMenuActivityPulse() {
    if (!S().pulseMenuActivity) return false;
    S().pulseMenuActivity = false;
    return true;
}

bool takeSendJustFinished() {
    if (!S().sendJustFinished) return false;
    S().sendJustFinished = false;
    return true;
}

// ─── App descriptor ─────────────────────────────────────────────────────────

static const ::scene::Screen* const kScreens[] = {
    &kScreenEyes,
    &kScreenRecording,
    &kScreenThinking,
    &kScreenReply,
    &kScreenError,
};
static AppPresentation presentationPolicy() {
    AppPresentation p{};
    if (motion::ownsStatusBackground()) p.statusBar=AppPresentation::StatusBar::GlyphOverlay;
    p.drawOverlay=[](TFT_eSprite&) { if(isRecording()) drawRecordingOverlay(); };
    return p;
}
static core::health::RuntimePolicy runtimePolicy() {
    const bool recording=isRecording();
    const bool midOperation=recording || isSending() || flow::waitingForDelegation();
    const bool reading=isOnReplyScreen() || isShowingError();
    core::health::RuntimePolicy policy{!recording, !midOperation, !(midOperation || reading), false};
    policy.allowBackgroundUploads = !midOperation;
    policy.allowAutomaticUpdate = isAtHomescreen();
    return policy;
}
static void prepareForSleep() {
    stopEyesFeedback();
    // Preserve the existing sleep contract: an in-flight turn is cancelled;
    // idle/reply/error state keeps its resources until the normal wake refresh.
    if (isRecording() || isSending()) {
        requestAbortSendingToEyes();
        exitApp();
    }
}
App app = [] {
    App descriptor = SCENE_APP_CATALOG_POLICIES("Kami", "kami", &kScreenEyes, exitApp,
                            "src/apps/agent_app.cpp", kScreens, presentationPolicy, runtimePolicy);
    descriptor.onCover = [] {
        stopEyesFeedback();
        flow::cancelDelegation();
        if(flow::isSessionActive()) services::agent_transport::cancel(static_cast<uint8_t>(CancelTarget::Eyes));
    };
    descriptor.onEnter = prepareHome;
    descriptor.onResume = resumeWorkflow;
    descriptor.onPrepareSleep = prepareForSleep;
    descriptor.onPreTick = preTick;
    return descriptor;
}();

}  // namespace agent
}  // namespace apps
