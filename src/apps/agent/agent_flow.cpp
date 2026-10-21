#include "apps/agent/agent_motion.h"
#include "services/agent_voice.h"
#include "apps/agent/agent_emotion.h"
#include "apps/agent/agent_flow.h"
#include "services/agent_coordination.h"
#include "services/agent_transport.h"
#include "services/agent_inbox.h"
#include "services/agent_delivery.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstring>
#include <utility>
#include "apps/settings_menu_app.h"


#include "apps/agent/agent_config.h"
#include "apps/agent_app.h"

#include "apps/agent/agent_render.h"
#include "apps/agent/agent_screens.h"
#include "config/app_config.h"
#include "config/ui_theme.h"
#include "core/memory.h"
#include "core/telemetry.h"
#include "services/agent_latency.h"
#include "hal/hal_display.h"
#include "hal/hal_wifi.h"
#include "hal/hal_input.h"
#include "services.h"
#include "scene/screen.h"
#include "sfx/sfx_typewriter.h"
#include "ui/agent_overlays.h"
#include "app.h"
#include "nav_handler.h"

namespace apps {
namespace agent {
namespace flow {

// ─── Singleton state ────────────────────────────────────────────────────────

static AgentState s_state;
static uint8_t s_pendingDelegations=0;
static uint32_t s_delegationStarted=0;

AgentState& state() { return s_state; }

namespace transport = services::agent_transport;

// ─── Transition table ────────────────────────────────────────────────────────

void goTo(Stage next) {
    if (s_state.stage == next) return;
    if(next==Stage::Error)services::agent_latency::fail();
    if(next==Stage::Eyes)services::agent_latency::cancel();
    if(next==Stage::Reply)services::agent_latency::replyReady();
    LOGD("agent", "stage %d -> %d\n", (int)s_state.stage, (int)next);
    motion::beforeStage(s_state.stage,next);
    s_state.prevStageForIdle = s_state.stage;
    s_state.stage            = next;
    NavHandler::instance().runtimePolicyChanged();
    if (next == Stage::Eyes) prepareHome();
    ::scene::switchTo(&app, &appScreenFor(next), next==Stage::Eyes ? ::scene::ScreenTransition::Back : ::scene::ScreenTransition::Forward);
}

// ─── Audio teardown helper ───────────────────────────────────────────────────

static void restoreAudio() {
    sfx::typewriterTeardown();
    services::agent_voice::restorePlayback();
}

// ─── Recording flow ──────────────────────────────────────────────────────────

bool wifiReady(){
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    if(s_state.wifiOverride>=0)return s_state.wifiOverride==1;
#endif
    return hal::wifi::connected()&&!hal::wifi::suspended();
}
void dismissNotice(){
    auto& s=s_state;
    if(s.hasPreviousReply){
        if(s.previousReply.anyNonEmpty()){
            s.conv.release();std::swap(s.conv,s.previousReply);
        }
        if(s.conv.anyNonEmpty()){
            s.msgIndex=s.previousMsgIndex;s.scrollLines=s.previousScrollLines;
            s.restoringReply=true;s.hasPreviousReply=false;goTo(Stage::Reply);return;
        }
    }
    returnToHome();
}
void openNoticeSettings(){
    s_state.resumeNotice=true;
    if(s_state.noticeKind==notice::Kind::Auth){NavHandler::instance().push(&apps::settings_menu::app);return;}
    apps::settings_menu::openWifiMenu();
}
void serviceNotice(uint32_t now){
    auto& s=s_state;if(s.stage!=Stage::Error)return;
    // Input can enter the screen after this frame's timestamp was sampled.
    const int32_t elapsed=int32_t(now-s.noticeSince);
    auto next=notice::connectionTick(s.noticeKind,wifiReady(),elapsed>0?uint32_t(elapsed):0);
    if(next!=s.noticeKind){s.noticeKind=next;render::paintError(s);}
    s.noticeInput.sample(hal::input::buttonADown(),hal::input::buttonBDown());
}

void startRecording() {
    services::agent_latency::begin();
    services::agent_delivery::dismiss();
    services::agent_inbox::setAccepting(true);
    s_pendingDelegations=0;
    if(s_state.stage==Stage::Reply){
        // Remember the view before capture takes ownership of a fresh buffer.
        s_state.previousReply.release();
        s_state.hasPreviousReply=s_state.conv.anyNonEmpty();
        s_state.previousMsgIndex=s_state.msgIndex;
        s_state.previousScrollLines=s_state.scrollLines;
    }
    if(!wifiReady()){
        s_state.recStartedByChord=false;s_state.chordHoldDeadlineMs=0;
        const bool pending=hal::wifi::connecting()||hal::wifi::suspended();
        snprintf(s_state.errBuf,sizeof(s_state.errBuf),"%s",pending?"Reconnecting Wi-Fi":cfg::agent::kErrWifi);
        // No microphone, audio slab, or transport session before this gate.
        if(s_state.stage==Stage::Error){
            s_state.noticeKind=notice::classify(s_state.errBuf,false);
            s_state.noticeSince=millis();s_state.noticeInput.reset();render::paintError(s_state);
        }else goTo(Stage::Error);
        return;
    }
    const uint32_t entryMs = millis();
    s_state.voiceEntryMs = entryMs;
    s_state.pcmN = 0;
    goTo(Stage::Recording);
    NavHandler::instance().draw(hal::display::canvas());
    hal::display::flush();
    const uint32_t visibleMs = millis() - entryMs;
    if (!services::agent_voice::prepare()) {
        LOGD("agent", "startRecording BAIL: resources::acquire failed");
        snprintf(s_state.errBuf, sizeof(s_state.errBuf), "%s", cfg::agent::kErrResourcesBusy);
        restoreAudio(); goTo(Stage::Error); return;
    }
    sfx::typewriterTeardown();
    render::releaseChatSprites(s_state);
    const auto started=services::agent_voice::start(transport::appendAudio);
    if (started != services::agent_voice::StartResult::Started) {
        const bool memory=started == services::agent_voice::StartResult::MemoryUnavailable;
        LOGD("agent", memory ? "startRecording BAIL: acquireBuffers failed" : "startRecording BAIL: mic.startStream failed");
        snprintf(s_state.errBuf, sizeof(s_state.errBuf), "%s", memory ? cfg::agent::kErrPsram : cfg::agent::kErrMic);
        restoreAudio(); goTo(Stage::Error); return;
    }

    s_state.pcmN = 0;
    if (!ensureTransport()) {
        LOGD("agent", "startRecording BAIL: ensureTransport failed");
        services::agent_voice::finish();
        services::agent_voice::release();
        snprintf(s_state.errBuf, sizeof(s_state.errBuf), "%s", "WebSocket task failed");
        restoreAudio();
        goTo(Stage::Error);
        return;
    }
    // Retain the last reply before the transport clears the next conversation.
    if(s_state.hasPreviousReply && !s_state.previousReply.anyNonEmpty() && s_state.conv.anyNonEmpty())
        std::swap(s_state.conv,s_state.previousReply);
    emotion::reset();
    transport::begin();
    services::agent_latency::milestone("mic_ready");
    Serial.printf("[voice-entry] visible_ms=%lu mic_ms=%lu\n",
                  (unsigned long)visibleMs, (unsigned long)(millis()-entryMs));

}

void finishRecording() {
    services::agent_latency::submit();
    // A server error may arrive before the user releases the buttons. Consume
    // it before checking/committing audio so its message cannot be overwritten.
    if (pollCompletion()) return;
    s_state.recStartedByChord = false;
    services::agent_voice::finish();
    services::agent_latency::milestone("mic_stopped");
    s_state.pcmN = services::agent_voice::samples();
    sfx::typewriterTeardown();
    startSend();
    if (s_state.stage == Stage::Error) return;
    goTo(Stage::Thinking);
}

namespace {
using SendOutcome = services::agent_transport::Outcome;
SendOutcome finishTransport(SendOutcome outcome, const char* error, uint32_t sentSamples) {
    AgentState& s = s_state;
    SendOutcome finalOutcome = outcome;
    if (outcome == SendOutcome::Cancelled) {
        s.cancelResult = CancelTarget::None;
        s.errBuf[0] = '\0';
        s.conv.release();
    } else if (outcome == SendOutcome::Ok) {
        LOGD("agent", "WS reply: segments=%d\n", s.conv.segmentCount());
        if (!s.conv.anyNonEmpty()) {
            snprintf(s.errBuf, sizeof(s.errBuf), "%s", cfg::agent::kErrEmptyReply);
            finalOutcome = SendOutcome::NetworkError;
            s.conv.release();
        } else {
            s.sendJustFinished = true;
        }
    } else {
        snprintf(s.errBuf, sizeof(s.errBuf), "%s", error && error[0] ? error : "WebSocket error");
        s.conv.release();
    }

    Serial.printf("[agent] finishSession outcome=%s final=%s err='%s' segments=%d nonempty=%d sent=%lu\n",
                  services::agent_transport::outcomeName(outcome),
                  services::agent_transport::outcomeName(finalOutcome),
                  s.errBuf,
                  s.conv.segmentCount(),
                  s.conv.anyNonEmpty() ? 1 : 0,
                  (unsigned long)sentSamples);

    if (services::agent_voice::isCapturing()) {
        s.recStartedByChord=false;
        s.pcmN=services::agent_voice::finish();
    }
    services::agent_voice::requestRelease();
    return finalOutcome;
}
int16_t* transportPcm() { return services::agent_voice::pcm(); }
uint32_t transportCapacity() { return services::agent_voice::capacity(); }
bool transportMessage(const char* text, bool append) {
    const bool ok=append ? s_state.conv.appendToken(text) : s_state.conv.pushMessage(text);
    if(ok) emotion::bind(s_state.conv.segmentCount()-1);
    return ok;
}
bool transportHasReply() { return s_state.conv.anyNonEmpty(); }
void transportStatus(const char* text) {
    snprintf(s_state.thinkStatus, sizeof(s_state.thinkStatus), "%s", text);
    s_state.thinkDotPhase = 255;
}
void transportDiscard(int segment) {
    AgentState& s = s_state;
    if (!s.conv.removeSegment(segment)) return;
    if (s.msgIndex >= s.conv.segmentCount()) s.msgIndex = s.conv.segmentCount() > 0 ? s.conv.segmentCount() - 1 : 0;
}
void transportEvent(const char* json) {
    JsonDocument doc;
    if (!deserializeJson(doc,json)) {
        if (!strcmp(doc["type"] | "", "delivery")) services::agent_delivery::bind(doc["receiptId"] | "",doc["segment"] | -1);
        else emotion::receive(doc.as<JsonVariantConst>(),true);
    }
}
const services::agent_transport::VoiceCallbacks kTransportCallbacks = {
    transportPcm, transportCapacity, transportMessage, transportHasReply,
    transportStatus, finishTransport, transportEvent, transportDiscard
};
} // namespace

bool ensureTransport(bool reconnectIfOffline) {
    return services::agent_transport::prepare(kTransportCallbacks, reconnectIfOffline);
}
void startSend() {
    if (pollCompletion()) return;
    AgentState& s = s_state;
    s.cancelResult      = CancelTarget::None;
    s.pulseMenuActivity = true;
    services::notifyUserActivity();
    if (transport::status().commitPending) return;

    if (s.pcmN == 0) {
        snprintf(s.errBuf, sizeof(s.errBuf), "%s", cfg::agent::kErrNoAudio);
        transport::abandon();
        restoreAudio(); services::agent_voice::release(); goTo(Stage::Error); return;
    }
    if (transport::status().overflowed) {
        Serial.printf("[agent] audio stream backlog overflow, dropped=%lu samples\n",
                      (unsigned long)transport::status().droppedSamples);
        snprintf(s.errBuf, sizeof(s.errBuf), "%s", cfg::agent::kErrAudioOverflow);
        transport::abandon();
        restoreAudio(); services::agent_voice::release(); goTo(Stage::Error); return;
    }
    if (!s.conv.ensure()) {
        snprintf(s.errBuf, sizeof(s.errBuf), "%s", cfg::agent::kErrConvBuf);
        transport::abandon();
        restoreAudio(); services::agent_voice::release(); goTo(Stage::Error); return;
    }

    s.conv.clear();
    transport::commit();
}

bool isTransportReady() { return transport::status().ready; }

bool isSessionActive() { return services::agent_coordination::isSessionActive(); }

bool isCommitPending() { return transport::status().commitPending; }

bool isCommitSent() { return transport::status().commitSent; }

static void showReplyIfReady() {
    AgentState& s = s_state;
    if (!transport::status().replyReady || s.stage != Stage::Thinking) return;
    if (!s.conv.ensure() || !s.conv.anyNonEmpty()) return;

    transport::clearReplyReady();
    restoreAudio();
    services::agent_voice::releaseBuffers();
    goTo(Stage::Reply);
}

void pollThinking(uint32_t visibleSinceMs) {
    if (transport::status().replyReady) {
        flow::showReplyIfReady();
        return;
    }
    if (pollCompletion()) return;

    const uint32_t timeoutNowMs = millis();
    const uint32_t elapsedMs = timeoutNowMs >= visibleSinceMs
        ? (timeoutNowMs - visibleSinceMs)
        : 0u;
    const uint32_t timeoutMs = flow::isCommitSent()
        ? (cfg::agent::kResponseTimeoutMs + 2000u)
        : (cfg::agent::kConnectGraceMs + 2000u);
    if (elapsedMs > timeoutMs) {
        services::agent_latency::fail();
        Serial.printf("[agent] thinking watchdog: commitSent=%d elapsed=%lu -> eyes\n",
                      flow::isCommitSent() ? 1 : 0,
                      (unsigned long)elapsedMs);
        flow::returnToHome();
    }
}


void cancelSend(CancelTarget nav) {
    if (s_state.stage == Stage::Recording) {
        transport::abandon();
        return;
    }
    transport::cancel(static_cast<uint8_t>(nav));
}

bool pollCompletion() {
    if (!transport::status().done) return false;
    services::agent_voice::poll();
    AgentState& s = s_state;
    const auto r = transport::result();
    transport::clearDone();

    switch (r.outcome) {
        case SendOutcome::Cancelled:
            restoreAudio();
            if (r.cancelTag == static_cast<uint8_t>(CancelTarget::Menu)) {
                goTo(Stage::Eyes);
                requestExitToMenu();
            } else {
                goTo(Stage::Eyes);
            }
            return true;

        case SendOutcome::Ok:
            awaitDelegatedReplies(r.pendingTasks);
            s.returnHomeAfterReply = false;
            s.returnHomeAtMs = 0;
            if (s.stage == Stage::Reply) {
                transport::clearDone();
                return true;
            }
            if (!s.conv.ensure()) {
                snprintf(s.errBuf, sizeof(s.errBuf), "%s", cfg::agent::kErrReplyUi);
                restoreAudio();
                goTo(Stage::Error);
                return true;
            }
            restoreAudio();
            goTo(Stage::Reply);
            return true;

        default:
            // Preserve the actual server/network diagnosis. In particular an
            // authentication rejection is not an audio-buffer overflow.
            Serial.printf("[agent] finalizeSend failed -> error: %s\n", s.errBuf);
            restoreAudio();
            goTo(Stage::Error);
            return true;
    }
}


// ─── Return to home ──────────────────────────────────────────────────────────

void returnToHome() {
    services::agent_latency::cancel();
    // Eyes is another view of Kami: keep the delegated job and its network lease.
    const bool keepDelegation = waitingForDelegation();
    if (keepDelegation) services::agent_delivery::replaceView();
    else cancelDelegation();
    if (s_state.stage == Stage::Recording) {
        s_state.recStartedByChord = false;
        services::agent_voice::finish();
        s_state.pcmN = services::agent_voice::samples();
        transport::abandon();
    } else if (s_state.stage == Stage::Thinking || s_state.stage == Stage::Reply) {
        transport::abandon();
        transport::clearDone();
        transport::clearReplyReady();
    }
    restoreAudio();

    hal::display::releaseSpiForSd();

    services::agent_voice::releaseBuffers();
    s_state.conv.release();
    s_state.previousReply.release();
    s_state.hasPreviousReply=false;s_state.restoringReply=false;s_state.resumeNotice=false;
    s_state.conv.clear();
    s_state.twRevealLen = 0;
    s_state.scrollLines = 0;
    s_state.msgIndex    = 0;
    s_state.errBuf[0]   = '\0';
    s_state.returnHomeAfterReply = false;
    s_state.returnHomeAtMs = 0;
    render::releaseChatSprites(s_state);

    if (!keepDelegation) services::agent_voice::releaseResources();
    goTo(Stage::Eyes);
}

bool showInboxReply() {
    AgentState& s = s_state;
    if ((s.stage != Stage::Eyes && s.stage != Stage::Reply) || s.chordHoldDeadlineMs != 0 || isSessionActive()) return false;
    if (!services::agent_inbox::hasReply() || !s.conv.ensure()) return false;
    return services::agent_inbox::takeReply([](const char* text, const char* receiptId) {
        AgentState& state = s_state;
        services::agent_delivery::replaceView();
        state.conv.clear();
        emotion::reset();
        if (!state.conv.pushMessage(text)) return false;
        emotion::bind(0);
        services::agent_delivery::bind(receiptId,0);
        if(s_pendingDelegations && --s_pendingDelegations==0) services::agent_voice::releaseResources();
        state.returnHomeAfterReply = false;
        state.returnHomeAtMs = 0;
        services::notifyUserActivity();
        if(state.stage==Stage::Reply) {
            NavHandler::instance().runtimePolicyChanged();
            ::scene::switchTo(&app,&appScreenFor(Stage::Reply));
        } else goTo(Stage::Reply);
        return true;
    });
}

void awaitDelegatedReplies(uint8_t count) {
    if(count && !services::agent_voice::prepare()) {
        Serial.println("[agent-delivery] could not reserve reply network resources");
    }
    s_pendingDelegations=count;
    s_delegationStarted=millis();
    services::agent_inbox::setAccepting(count>0);
    NavHandler::instance().runtimePolicyChanged();
    Serial.printf("[agent-delivery] waiting=%u\n",(unsigned)count);
}
bool waitingForDelegation() { return s_pendingDelegations != 0; }
void cancelDelegation() {
    const bool wasWaiting=s_pendingDelegations!=0;
    s_pendingDelegations=0;
    if(wasWaiting){services::agent_voice::releaseResources();NavHandler::instance().runtimePolicyChanged();}
    services::agent_inbox::setAccepting(false);
    services::agent_delivery::dismiss();
}
void pollDelegation(uint32_t now) {
    if(s_pendingDelegations && int32_t(now-s_delegationStarted)>15*60*1000) cancelDelegation();
}
void acknowledgeDisplayed() {
    if(s_state.stage==Stage::Reply && s_state.twRevealLen>0)
        services::agent_delivery::displayed(s_state.msgIndex);
}

void pollReturnHomeAfterReply(uint32_t nowMs, bool revealed) {
    AgentState& s = s_state;
    if (!s.returnHomeAfterReply || s.stage != Stage::Reply) return;
    if (!revealed || s.msgIndex + 1 < s.conv.segmentCount()) { s.returnHomeAtMs = 0; return; }
    if (s.returnHomeAtMs == 0) { s.returnHomeAtMs = nowMs + cfg::agent::kReturnHomeAfterHandoffMs; return; }
    if ((int32_t)(nowMs - s.returnHomeAtMs) < 0) return;
    s.returnHomeAfterReply = false;
    returnToHome();
}

void keepReply() {
    s_state.returnHomeAfterReply = false;
    s_state.returnHomeAtMs = 0;
}

uint8_t micLevel() { return services::agent_voice::waveform().level(); }

}  // namespace flow
}  // namespace agent
}  // namespace apps
