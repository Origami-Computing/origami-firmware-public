#include "services/tls_trust.h"
#include "services/agent_transport.h"
#include "services/background_network.h"
#include "services/agent_reply_mailbox.h"
#include "services/agent_inbox.h"
#include "services/agent_delivery.h"
#include "core/conversation_buffer.h"
#include "core/memory.h"
#include "services/agent_transport_config.h"
#include "services/agent_coordination.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <atomic>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/task.h>
#include "core/device_agent_text.h"
#include "core/device_id.h"
#include "core/secrets.h"
#include "core/telemetry.h"
#include "hal/hal_tasks.h"
#include "hal/hal_wifi.h"
#include "services.h"
namespace services { namespace agent_transport {
// ─── Persistent WebSocket transport ──────────────────────────────────────────
//
// The Core 0 worker stays connected between turns. During Recording it reads
// the monotonically growing PCM buffer and sends only the new tail as binary
// frames. On release, startSend() sends `commit`; agent events return as JSON
// text frames on the same socket.

namespace {
using SendOutcome = Outcome;
using SendResult = Result;
std::atomic<const VoiceCallbacks*> s_callbacks{nullptr};
int16_t* pcm() { const auto* c=s_callbacks.load(); return c ? c->pcm() : nullptr; }
uint32_t capacity() { const auto* c=s_callbacks.load(); return c ? c->capacity() : 0; }
ReplyMailbox s_replies;
bool s_uiCancelled=false, s_uiRejected=false;
uint8_t s_uiCancelTag=0;
static bool s_replyReceived=false;
static TaskHandle_t    s_sendHandle = nullptr;
static bool   s_sendDone   = false;
static bool   s_replyReady = false;
static bool   s_sendCancel = false;
static uint8_t s_sendCancelNav = 0;
static SendResult      s_sendResult;
static WebSocketsClient s_socket;
static bool    s_socketConnected = false;
static bool    s_serverReady = false; // Authenticated ready frame, under s_audioMux.
static bool    s_listening = false; // Server acknowledged this turn's start.
// WS lifecycle: configured once when WiFi first comes up, then auto-reconnects
// to that host. `s_reconfigureRequest` lets another task (the dev/prod toggle)
// ask kamiSendTask to drop the socket and take a new backendConfig() snapshot — see
// reconfigure(). File-scope (was task-local) so it survives across the
// task loop and can be reset externally.
static bool    s_socketConfigured  = false;
static std::atomic<bool> s_reconfigureRequest{false};
static uint32_t         s_apiKeyRevision = 0; // Worker-owned observed revision.
static bool    s_commitRequested = false;
static bool    s_startSent       = false;
static bool    s_commitSent      = false;
static uint32_t s_sentSamples    = 0;
static uint32_t s_commitAtMs     = 0;
static bool    s_transportLost   = false;
static uint32_t         s_lastTransportWaitLogMs = 0;
// millis() when the socket last became connected — used to require a stable link
// before streaming audio (see pumpAudio / kWsStableMs).
static uint32_t s_wsConnectedSinceMs = 0;
static portMUX_TYPE     s_audioMux        = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_turnEpoch = 0; // Audio lock: identity, ring and all shared turn progress.
static bool s_audioAttached = false; // Protected by s_audioMux, including pointer lookup.
static uint32_t s_ringRead       = 0;
static uint32_t s_ringWrite      = 0;
static uint32_t s_ringUsed       = 0;
static uint32_t s_streamDropped  = 0;
static bool    s_streamOverflow  = false;
static int16_t          s_wsFrame[cfg::agent::kAudioFrameBytes / sizeof(int16_t)];
// Protocol extensions the server advertised in `ready`; `hello` opts in.
static bool s_helloPending = false;

// stream_v1 drafts (worker task only). A draft is one message the server
// streams as tokens before committing it; the committed `message` carries the
// same draftId and a withdrawn draft is removed. Segment indexes mirror the
// UI ConversationBuffer: every Message pushed this epoch is one segment.
struct DraftSlot { char id[64]; int segment; };
static constexpr int kMaxDrafts = 8;
static DraftSlot s_drafts[kMaxDrafts];
static int s_draftCount = 0;
static int s_replySegments = 0;
static uint32_t s_draftEpoch = 0;
static char s_wsHost[160]    = {};
static char s_wsPath[224]    = {};
static char s_wsHeaders[256] = {};
static uint16_t s_wsPort     = 0;
static bool s_wsSecure       = false;


void syncDraftEpoch(uint32_t epoch) {
    if (epoch == s_draftEpoch) return;
    s_draftEpoch = epoch;
    s_draftCount = 0;
    s_replySegments = 0;
}

int findDraft(const char* id) {
    for (int i = 0; i < s_draftCount; ++i) if (!strcmp(s_drafts[i].id, id)) return i;
    return -1;
}

void dropDraft(int index) {
    for (int i = index; i + 1 < s_draftCount; ++i) s_drafts[i] = s_drafts[i + 1];
    s_draftCount--;
}

bool receiveMessage(uint32_t epoch, const char* text, bool append) {
    const auto result=s_replies.push(epoch, append ? ReplyMailbox::Type::Token : ReplyMailbox::Type::Message, text);
    if (result==ReplyMailbox::Push::Queued && text && text[0]) {
        portENTER_CRITICAL(&s_audioMux);
        if (epoch == s_turnEpoch && services::agent_coordination::isSessionActive()) s_replyReceived=true;
        portEXIT_CRITICAL(&s_audioMux);
    }
    return result!=ReplyMailbox::Push::Full;
}

// Caller holds s_audioMux; shared terminal and abandon cleanup.
void resetSessionFlags() {
    services::agent_coordination::setSessionActive(false);
    s_commitRequested = false;
    s_startSent       = false;
    s_listening       = false;
    s_commitSent      = false;
    s_sentSamples     = 0;
    s_commitAtMs      = 0;
    s_transportLost   = false;
    s_sendCancel      = false;
}

// Caller holds s_audioMux, which publishes link time and connected state together.
bool socketReadyForAudioLocked(uint32_t nowMs) {
    return s_socketConnected && s_serverReady &&
           (uint32_t)(nowMs - s_wsConnectedSinceMs) >= cfg::agent::kWsStableMs;
}

// Caller holds s_audioMux. Activation/reset cannot expose an old epoch with a new ring.
void resetAudioRingLocked() {
    s_ringRead = 0;
    s_ringWrite = 0;
    s_ringUsed = 0;
    s_streamDropped = 0;
    s_streamOverflow = false;
}

bool streamMicSamples(const int16_t* samples, uint32_t count, void*) {
    if (!samples || count == 0) return true;

    bool acceptedAll = true;
    bool overflowStarted = false;
    portENTER_CRITICAL(&s_audioMux);
    int16_t* ring = s_audioAttached ? pcm() : nullptr;
    const uint32_t cap = s_audioAttached ? capacity() : 0;
    if (!ring || cap == 0) {
        s_streamOverflow = true;
        portEXIT_CRITICAL(&s_audioMux);
        return false;
    }
    uint32_t freeSamples = cap - s_ringUsed;
    if (count > freeSamples) {
        overflowStarted = !s_streamOverflow;
        s_streamOverflow = true;
        s_streamDropped += (count - freeSamples);
        count = freeSamples;
        acceptedAll = false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        ring[s_ringWrite] = samples[i];
        s_ringWrite = (s_ringWrite + 1) % cap;
    }
    s_ringUsed += count;
    const uint32_t used = s_ringUsed;
    portEXIT_CRITICAL(&s_audioMux);
    if (overflowStarted) {
        LOGD("agent", "audio buffer overflow: ring full at %lu/%lu samples; network is not draining\n",
                      (unsigned long)used, (unsigned long)cap);
    }
    return acceptedAll;
}

uint32_t peekAudioFrame(uint32_t epoch, int16_t* out, uint32_t maxSamples) {
    if (!out || maxSamples == 0) return 0;
    portENTER_CRITICAL(&s_audioMux);
    if (epoch != s_turnEpoch || !services::agent_coordination::isSessionActive()) {
        portEXIT_CRITICAL(&s_audioMux);
        return 0;
    }
    int16_t* ring = s_audioAttached ? pcm() : nullptr;
    const uint32_t cap = s_audioAttached ? capacity() : 0;
    if (!ring || cap == 0) {
        portEXIT_CRITICAL(&s_audioMux);
        return 0;
    }
    const uint32_t count = s_ringUsed < maxSamples ? s_ringUsed : maxSamples;
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = ring[(s_ringRead + i) % cap];
    }
    portEXIT_CRITICAL(&s_audioMux);
    return count;
}

bool dropAudioFrame(uint32_t epoch, uint32_t count) {
    portENTER_CRITICAL(&s_audioMux);
    const bool current = epoch == s_turnEpoch && services::agent_coordination::isSessionActive();
    const uint32_t cap = current && s_audioAttached ? capacity() : 0;
    if (cap == 0) {
        portEXIT_CRITICAL(&s_audioMux);
        return false;
    }
    if (count > s_ringUsed) count = s_ringUsed;
    s_ringRead = (s_ringRead + count) % cap;
    s_ringUsed -= count;
    s_sentSamples += count;
    portEXIT_CRITICAL(&s_audioMux);
    return true;
}

// Try to rewind the ring so a session can re-stream from the start after the
// socket dropped mid-send. The whole take is recoverable only if it still fits in
// the ring (short clip, never overflowed): the bytes we already "sent" are still
// physically present at [0, total) because the writer hasn't wrapped over them.
// Returns true if the rewind succeeded (caller should re-send), false if the
// audio is unrecoverable (overflowed / wrapped) and the turn must fail.
// Caller holds s_audioMux through the disconnect decision and rewind.
bool rewindRingForResendLocked() {
    bool ok = false;
    const uint32_t cap   = s_audioAttached ? capacity() : 0;
    const uint32_t total = s_sentSamples + s_ringUsed;  // captured this session
    if (!s_streamOverflow && cap > 0 && total <= cap) {
        s_ringRead    = 0;
        s_ringUsed    = total;
        s_sentSamples = 0;
        ok = true;
    }
    return ok;
}

// Decide what happens to the active session when the socket goes away (explicit
// disconnect, WiFi down, camera suspend). Recoverable → rewind so it re-streams on
// reconnect; otherwise mark the turn lost. Fatal only when the reply is already in
// flight (commit reached the server / tokens arriving) or the take overflowed the
// ring. Safe to call repeatedly; both callers run on the transport task.
void handleSessionDrop() {
    portENTER_CRITICAL(&s_audioMux);
    s_socketConnected = false;
    s_serverReady = false;
    s_listening = false;
    s_startSent = false;
    if (services::agent_coordination::isSessionActive() &&
        (s_commitSent || s_replyReceived || !rewindRingForResendLocked())) s_transportLost = true;
    s_commitSent = false;
    portEXIT_CRITICAL(&s_audioMux);
}

void finishSession(SendOutcome outcome, const char* error, uint32_t eventEpoch, uint8_t pendingTasks = 0) {
    ReplyMailbox::Completion completion;
    completion.outcome=outcome;
    completion.pendingTasks=pendingTasks;
    snprintf(completion.error,sizeof(completion.error),"%s",error ? error : "");
    // Activation and cleanup share one identity check. Never hold this lock
    // across mailbox allocation/free, callbacks, socket work or publication.
    portENTER_CRITICAL(&s_audioMux);
    if (eventEpoch != s_turnEpoch || !services::agent_coordination::isSessionActive()) {
        portEXIT_CRITICAL(&s_audioMux);
        return;
    }
    completion.cancelTag=outcome==SendOutcome::Cancelled ? s_sendCancelNav : 0;
    completion.sentSamples=s_sentSamples;
    resetSessionFlags();
    portEXIT_CRITICAL(&s_audioMux);
    // Publish after cleanup; a concurrent UI reset still rejects this epoch.
    LOGI("agent-turn", "e=%lu up=%lu end=%u sent=%lu", (unsigned long)eventEpoch,
         (unsigned long)millis(), (unsigned)outcome, (unsigned long)completion.sentSamples);
    // Error text only, in bounded chunks; never log audio, prompts or credentials.
    for (size_t offset=0; offset<strlen(completion.error); offset+=48)
        LOGI("agent-error", "e=%lu part=%u %.48s", (unsigned long)eventEpoch,
             (unsigned)(offset/48), completion.error+offset);
    s_replies.finish(eventEpoch,completion);
}

struct TurnSnapshot {
    uint32_t epoch, commitAt, sent, queued;
    bool active, cancel, commitRequested, commitSent, startSent, lost;
};
TurnSnapshot turnSnapshot() {
    portENTER_CRITICAL(&s_audioMux);
    const TurnSnapshot result{s_turnEpoch, s_commitAtMs, s_sentSamples, s_ringUsed,
        services::agent_coordination::isSessionActive(), s_sendCancel,
        s_commitRequested, s_commitSent, s_startSent, s_transportLost};
    portEXIT_CRITICAL(&s_audioMux);
    return result;
}

bool parseBackendUrl(const core::secrets::BackendConfig& config) {
    const char* raw = config.url;
    if (!raw || !raw[0]) return false;

    const char* host = raw;
    if (!strncmp(raw, "https://", 8)) {
        s_wsSecure = true;
        s_wsPort = 443;
        host = raw + 8;
    } else if (!strncmp(raw, "http://", 7)) {
        s_wsSecure = false;
        s_wsPort = 80;
        host = raw + 7;
    } else {
        return false;
    }

    const char* slash = strchr(host, '/');
    const char* hostEnd = slash ? slash : host + strlen(host);
    const char* colon = nullptr;
    for (const char* p = host; p < hostEnd; ++p) {
        if (*p == ':') colon = p;
    }
    const char* nameEnd = colon ? colon : hostEnd;
    const size_t hostLen = (size_t)(nameEnd - host);
    if (hostLen == 0 || hostLen >= sizeof(s_wsHost)) return false;
    memcpy(s_wsHost, host, hostLen);
    s_wsHost[hostLen] = '\0';
    if (colon) s_wsPort = (uint16_t)atoi(colon + 1);

    if (slash && slash[1]) {
        const size_t baseLen = strlen(slash);
        const bool trailingSlash = baseLen > 0 && slash[baseLen - 1] == '/';
        snprintf(s_wsPath, sizeof(s_wsPath), "%s%s%s", slash,
                 trailingSlash ? "" : "/", cfg::agent::kWebSocketPath + 1);
    } else {
        snprintf(s_wsPath, sizeof(s_wsPath), "%s", cfg::agent::kWebSocketPath);
    }
    snprintf(s_wsHeaders, sizeof(s_wsHeaders),
             "X-Device-Id: %s\r\nX-Api-Key: %s",
             core::device::id(), config.apiKey);
    return s_wsPort != 0;
}

// A failed send may have written a frame prefix. Never append a fresh frame to
// that byte stream: close it and let the existing ring rewind recover the take.
void disconnectAfterWriteFailure(const char* frame) {
    LOGW("agent", "WS write failed frame=%s", frame);
    s_socket.disconnect();
    // Also invalidate state if the library had already cleaned up the socket
    // and therefore emitted no new disconnect callback.
    handleSessionDrop();
}

void sendStartFrame(uint32_t epoch) {
    char json[192];
    snprintf(json, sizeof(json),
             "{\"type\":\"start\",\"sessionId\":\"%s\",\"platform\":\"%s\",\"sampleRate\":16000}",
             cfg::agent::kSessionId, cfg::agent::kPlatform);
    if (s_socket.sendTXT(json)) {
        portENTER_CRITICAL(&s_audioMux);
        if (epoch == s_turnEpoch && services::agent_coordination::isSessionActive()) s_startSent = true;
        portEXIT_CRITICAL(&s_audioMux);
        LOGD("agent", "WS start");
    } else {
        disconnectAfterWriteFailure("start");
    }
}

void handleSocketText(uint8_t* payload, size_t length) {
    const auto replyEpoch=s_replies.epoch();
    JsonDocument doc;
    if (deserializeJson(doc, payload, length)) {
        LOGD("agent", "WS text parse failed len=%u\n", (unsigned)length);
        return;
    }

    const char* receipt = doc["receiptId"] | "";
    const char* incomingType = doc["type"] | "";
    if (!strcmp(incomingType,"delivery_ack_recorded")) {
        agent_delivery::confirmed(receipt, doc["stage"] | ""); return;
    }
    if (receipt[0]) agent_delivery::received(receipt);
    if (!strcmp(incomingType,"committed")) {
        agent_delivery::bind(receipt,-2); // Commit is a receipt, never screen content.
        Serial.printf("[agent-delivery] commit_accepted bytes=%lu id=%s\n",(unsigned long)(doc["bytes"] | 0u),receipt);
        return;
    }
    if (core::device_agent_text::receive(doc)) return;
    const char* type = doc["type"] | "";
    if (!strcmp(type,"ready")) {
        portENTER_CRITICAL(&s_audioMux);
        s_serverReady = s_socketConnected;
        portEXIT_CRITICAL(&s_audioMux);
        LOGD("agent", "WS ready");
        bool supported=false;
        for (JsonVariantConst capability : doc["capabilities"].as<JsonArrayConst>())
            if (!strcmp(capability.as<const char*>() ? capability.as<const char*>() : "", "text_v1")) supported=true;
        core::device_agent_text::ready(supported);
        // Opt into pushes, streaming and widget frames; servers that do not
        // advertise them ignore the unknown `hello` harmlessly.
        s_helloPending = !doc["capabilities"].isNull();
        Serial.printf("[agent-text] capability=%u\n",supported);
        return;
    }
    // Server-initiated frames: valid with or without an active voice turn.
    if (!strcmp(type,"push")) {
        (void)services::agent_inbox::offerReply(doc["eventId"] | (uint64_t)0, doc["text"] | "", receipt);
        return;
    }
    if (!strcmp(type,"widget")) {
        const char* action = doc["action"] | "";
        const bool ready = !strcmp(action, "widget_ready");
        (void)services::agent_inbox::offerWidget(action,
            ready ? (doc["id"] | "") : !strcmp(action, "widget_error") ? (doc["message"] | "") : (doc["title"] | ""),
            ready ? (doc["slug"] | "") : "");
        return;
    }
    if (!services::agent_coordination::isSessionActive()) { agent_delivery::reject(receipt,"not_waiting"); return; }
    if (!strcmp(type, "listening")) {
        portENTER_CRITICAL(&s_audioMux);
        if (replyEpoch == s_turnEpoch && s_startSent) s_listening = true;
        portEXIT_CRITICAL(&s_audioMux);
    }
    if (!strcmp(type, "execution_start"))
        LOGI("agent-exec", "e=%lu up=%lu id=%.36s", (unsigned long)replyEpoch,
             (unsigned long)millis(), doc["executionId"] | "");
    syncDraftEpoch(replyEpoch);
    LOGD("agent-event", "e=%lu up=%lu rx=%.24s n=%u", (unsigned long)replyEpoch,
         (unsigned long)millis(), type, (unsigned)length);

    if (!strcmp(type,"emotion") || !strcmp(type,"tool_start") || !strcmp(type,"tool_end")) {
        if (!doc["requestId"].isNull()) return;
        // Re-serialize a bounded document: incoming payload need not be NUL-terminated.
        const size_t size=measureJson(doc)+1;
        if (size>2048) return;
        void* storage=core::memory::allocPsram("agent.metadata",size);
        if(!storage) { finishSession(SendOutcome::NetworkError,"Reply buffer full",replyEpoch);return; }
        serializeJson(doc,static_cast<char*>(storage),size);
        const auto queued=s_replies.push(replyEpoch,ReplyMailbox::Type::Metadata,static_cast<char*>(storage));
        core::memory::freePsram(storage);
        if (queued == ReplyMailbox::Push::Full) finishSession(SendOutcome::NetworkError,"Reply buffer full",replyEpoch);
    } else if (!strcmp(type, "message")) {
        // Internal messages are the agent's notes, never part of the reply.
        if (doc["is_internal"] | false) return;
        const char* text = doc["text"] | "";
        const char* draftId = doc["draftId"] | "";
        LOGD("agent", "WS message chars=%u draft=%s\n", (unsigned)strlen(text), draftId);
        const int draft = draftId[0] ? findDraft(draftId) : -1;
        if (receipt[0]) {
            char metadata[160];
            const int segment = draft >= 0 ? s_drafts[draft].segment : s_replySegments;
            snprintf(metadata,sizeof(metadata),"{\"type\":\"delivery\",\"receiptId\":\"%s\",\"segment\":%d}",receipt,segment);
            if(s_replies.push(replyEpoch,ReplyMailbox::Type::Metadata,metadata)!=ReplyMailbox::Push::Queued)
                agent_delivery::reject(receipt,"buffer_full");
        }
        if (draft >= 0) { dropDraft(draft); return; } // Already on screen as streamed.
        if (!receiveMessage(replyEpoch,text,false)) {
            agent_delivery::reject(receipt,"buffer_full");
            finishSession(SendOutcome::NetworkError, "Reply buffer full", replyEpoch);
            return;
        }
        if (s_replySegments < core::ConversationBuffer::maxSegments()) s_replySegments++;
    } else if (!strcmp(type, "token")) {
        const char* text = doc["text"] | "";
        const char* draftId = doc["draftId"] | "";
        LOGD("agent", "WS token chars=%u\n", (unsigned)strlen(text));
        bool append = true;
        if (draftId[0] && findDraft(draftId) < 0) {
            // A new draft becomes its own segment. With no room to track it,
            // skip its tokens; the committed message still arrives in full.
            if (s_draftCount >= kMaxDrafts || s_replySegments >= core::ConversationBuffer::maxSegments()) return;
            strlcpy(s_drafts[s_draftCount].id, draftId, sizeof(s_drafts[s_draftCount].id));
            s_drafts[s_draftCount].segment = s_replySegments++;
            s_draftCount++;
            append = false;
        }
        if (!receiveMessage(replyEpoch,text,append)) {
            finishSession(SendOutcome::NetworkError, "Reply buffer full", replyEpoch);
        }
    } else if (!strcmp(type, "draft_discard")) {
        const int draft = findDraft(doc["draftId"] | "");
        if (draft < 0) return;
        const int segment = s_drafts[draft].segment;
        dropDraft(draft);
        for (int i = 0; i < s_draftCount; ++i) if (s_drafts[i].segment > segment) s_drafts[i].segment--;
        s_replySegments--;
        char index[12];
        snprintf(index, sizeof(index), "%d", segment);
        if (s_replies.push(replyEpoch, ReplyMailbox::Type::Discard, index) == ReplyMailbox::Push::Full)
            finishSession(SendOutcome::NetworkError, "Reply buffer full", replyEpoch);
    } else if (!strcmp(type, "status")) {
        Serial.printf("[agent] WS status: %s\n", doc["text"] | "Working");
        if (s_replies.push(replyEpoch, ReplyMailbox::Type::Status,
                           doc["text"] | "Working") == ReplyMailbox::Push::Full)
            finishSession(SendOutcome::NetworkError, "Reply buffer full", replyEpoch);
    } else if (!strcmp(type, "done")) {
        const uint8_t pending = (uint8_t)(doc["pendingTasks"] | 0);
        LOGD("agent", "WS done pendingTasks=%u\n", (unsigned)pending);
        finishSession(SendOutcome::Ok, nullptr, replyEpoch, pending);
    } else if (!strcmp(type, "error")) {
        const char* message = doc["message"] | "Server error";
        Serial.printf("[agent] WS error: %s\n", message);
        finishSession(SendOutcome::NetworkError, message, replyEpoch);
    } else if (!strcmp(type, "cancelled")) {
        LOGD("agent", "WS cancelled");
        finishSession(SendOutcome::Cancelled, nullptr, replyEpoch);
    } else {
        Serial.printf("[agent] WS ignored type=%s\n", type);
    }
}

void socketEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED: {
            const uint32_t connectedAt = millis();
            portENTER_CRITICAL(&s_audioMux);
            s_socketConnected = true;
            s_serverReady = false;
            s_listening = false;
            s_wsConnectedSinceMs = connectedAt;
            s_startSent = false;
            s_commitSent = false;
            portEXIT_CRITICAL(&s_audioMux);
            Serial.printf("[agent] WS connected: %s\n", payload ? (char*)payload : "");
            break;
        }
        case WStype_DISCONNECTED:
            agent_delivery::disconnected();
            core::device_agent_text::disconnected();
            s_helloPending = false;
            // Recover a dropped session instead of failing it: the whole take is
            // still in the local PCM ring, so rewind and re-stream on reconnect.
            // This is what makes the first recording after boot work (the link
            // often drops right at commit). Only fatal once the reply is in flight
            // or the take overflowed the ring. (See handleSessionDrop.)
            handleSessionDrop();
            // Each record fits telemetry's 95-character payload. The old long
            // line truncated PSRAM; the close reason was never recorded.
            LOGW("agent", "WS drop reason=%.*s wifi=%d rssi=%d", (int)(length > 40 ? 40 : length),
                 payload ? (const char*)payload : "", hal::wifi::connected() ? 1 : 0,
                 hal::wifi::connected() ? WiFi.RSSI() : 0);
            LOGD("agent", "WS heap free=%u largest=%u psram=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            break;
        case WStype_TEXT:
            handleSocketText(payload, length);
            break;
        case WStype_ERROR:
            LOGD("agent", "WS protocol error");
            break;
        default:
            break;
    }
}

void configureSocket() {
    core::device_agent_text::disconnected();
    const auto config = core::secrets::backendConfig();
    if (!parseBackendUrl(config)) {
        Serial.printf("[agent] invalid backend URL for WS: %s\n", config.url);
        return;
    }
    portENTER_CRITICAL(&s_audioMux);
    s_socketConnected = false;
    s_wsConnectedSinceMs = 0;
    s_serverReady = false;
    s_listening = false;
    s_startSent = false;
    s_commitSent = false;
    portEXIT_CRITICAL(&s_audioMux);
    s_socket.onEvent(socketEvent);
    s_socket.setExtraHeaders(s_wsHeaders);
    s_socket.setReconnectInterval(1200);
    // Keepalive tuned for this backend, which (a) closes idle connections after
    // ~18s and (b) doesn't send pong replies. So: ping every 8s — frequent enough
    // to keep the server's idle timer reset (the pre-warmed socket and the post-
    // commit "thinking" wait both stay alive) — but tolerate a huge number of
    // missing pongs so the client never tears the healthy connection down itself.
    // (3rd arg is uint8_t; 255 = effectively "never self-disconnect on pong loss".)
    s_socket.enableHeartbeat(8000, 3000, 255);
    if (s_wsSecure) s_socket.beginSslWithBundle(s_wsHost, s_wsPort, s_wsPath, services::tls::rootBundle());
    else s_socket.begin(s_wsHost, s_wsPort, s_wsPath);
    Serial.printf("[agent] WS target %s://%s:%u%s auth=api-key device=%s\n",
                  s_wsSecure ? "wss" : "ws", s_wsHost, (unsigned)s_wsPort, s_wsPath,
                  core::device::id());
}

void pumpAudio() {
    // One pump belongs to one turn, including across blocking socket calls.
    const uint32_t now = millis();
    portENTER_CRITICAL(&s_audioMux);
    const bool ready = socketReadyForAudioLocked(now);
    const uint32_t epoch = s_turnEpoch;
    const bool active = services::agent_coordination::isSessionActive();
    const bool startNeeded = !s_startSent;
    portEXIT_CRITICAL(&s_audioMux);
    if (!active || !ready) return;
    if (startNeeded) sendStartFrame(epoch);
    portENTER_CRITICAL(&s_audioMux);
    const bool started = epoch == s_turnEpoch && services::agent_coordination::isSessionActive() && s_startSent;
    portEXIT_CRITICAL(&s_audioMux);
    if (!started) return;
    // Do not flood a just-opened connection before its start acknowledgment.
    portENTER_CRITICAL(&s_audioMux);
    const bool listening = epoch == s_turnEpoch && s_listening;
    portEXIT_CRITICAL(&s_audioMux);
    if (!listening) return;

    const uint32_t maxSamplesPerFrame = cfg::agent::kAudioFrameBytes / sizeof(int16_t);
    // Bound each drain pass even when capture produces faster than the socket.
    // Return to socket.loop() for ready/error/ping frames, and to the worker's
    // cancellation/deadline checks. A single socket write may still block.
    for (uint32_t frame = 0; frame < cfg::agent::kAudioFramesPerPump; ++frame) {
        portENTER_CRITICAL(&s_audioMux);
        const bool stopped = epoch != s_turnEpoch || s_sendCancel || s_transportLost;
        portEXIT_CRITICAL(&s_audioMux);
        if (stopped || services::agent_coordination::suspendRequested()) return;
        const uint32_t count = peekAudioFrame(epoch, s_wsFrame, maxSamplesPerFrame);
        if (count == 0) break;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(s_wsFrame);
        if (!s_socket.sendBIN(bytes, count * sizeof(int16_t))) {
            disconnectAfterWriteFailure("audio");
            return;
        }
        if (!dropAudioFrame(epoch, count)) return;
    }

    portENTER_CRITICAL(&s_audioMux);
    const bool commitNeeded = epoch == s_turnEpoch && services::agent_coordination::isSessionActive() &&
        s_commitRequested && !s_commitSent && !s_sendCancel && !s_transportLost && s_ringUsed == 0;
    portEXIT_CRITICAL(&s_audioMux);
    if (commitNeeded && !services::agent_coordination::suspendRequested()) {
        if (!s_socket.sendTXT("{\"type\":\"commit\"}")) {
            // Delivery is ambiguous: do not replay a potentially executed query.
            finishSession(SendOutcome::NetworkError, "Could not confirm audio submission", epoch);
            disconnectAfterWriteFailure("commit");
            return;
        }
        portENTER_CRITICAL(&s_audioMux);
        const bool current = epoch == s_turnEpoch && services::agent_coordination::isSessionActive();
        if (current) s_commitSent = true;
        const uint32_t sent = s_sentSamples;
        portEXIT_CRITICAL(&s_audioMux);
        if (current) LOGD("agent", "WS committed %lu samples\n", (unsigned long)sent);
    }
}

bool sendTextRequest(const char* wire) {
    if (s_socket.sendTXT(wire)) return true;
    disconnectAfterWriteFailure("text");
    return false;
}

void sendHelloIfPending() {
    if (!s_helloPending || !s_socketConnected) return;
    char json[192];
    const uint64_t after = services::agent_inbox::lastReplyEventId();
    if (after) snprintf(json, sizeof(json),
        "{\"type\":\"hello\",\"capabilities\":[\"push_v1\",\"stream_v1\",\"widget_v1\",\"delivery_ack_v1\"],\"afterEventId\":%llu}",
        (unsigned long long)after);
    else snprintf(json, sizeof(json),
        "{\"type\":\"hello\",\"capabilities\":[\"push_v1\",\"stream_v1\",\"widget_v1\",\"delivery_ack_v1\"]}");
    if (s_socket.sendTXT(json)) {
        s_helloPending = false;
        Serial.printf("[agent] WS hello after=%llu\n", (unsigned long long)after);
    } else {
        disconnectAfterWriteFailure("hello");
    }
}

void kamiSendTask(void*) {
    services::agent_coordination::bindCurrentWorker();
    using coordination = services::agent_coordination::WorkerPhase;
    for (;;) {
        services::agent_coordination::setWorkerPhase(coordination::TextPump);
        core::device_agent_text::pump(millis(),
            s_socketConnected && !services::agent_coordination::suspendRequested() && !hal::wifi::suspended() && hal::wifi::connected(),
            services::agent_coordination::isSessionActive(),sendTextRequest);

        if (services::agent_coordination::suspendRequested()) {
            services::agent_coordination::setWorkerPhase(coordination::PauseCleanup);
            if (s_socketConfigured) {
                s_socket.disconnect();
                s_socketConfigured = false;
                handleSessionDrop();
            }
            services::agent_coordination::acknowledgeSuspended();
            services::agent_coordination::setWorkerPhase(coordination::Waiting);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        // Honor a user cancel/quit EVERY iteration, before the WiFi-down branch
        // below can `continue` past it. Previously the cancel check lived only in
        // the connected path, so a quit pressed while WiFi was down/suspended
        // (common right after the camera, or during a flaky reconnect) sat pending
        // — the screen looked frozen ("can't quit") and then "suddenly" jumped to
        // eyes the moment WiFi returned and the loop finally reached the handler.
        // Offline we just can't notify the server; the local session aborts now.
        auto turn = turnSnapshot();
        // Worker-owned, at most one progress sample per second per active turn.
        static uint32_t lastProgress=0, lastEpoch=0;
        const uint32_t progressNow=millis();
        if (turn.active && (lastEpoch!=turn.epoch || uint32_t(progressNow-lastProgress)>=1000)) {
            lastProgress=progressNow; lastEpoch=turn.epoch;
            LOGI("agent-progress", "e=%lu up=%lu q=%lu sent=%lu start=%u commit=%u",
                 (unsigned long)turn.epoch, (unsigned long)progressNow,
                 (unsigned long)turn.queued, (unsigned long)turn.sent,
                 (unsigned)turn.startSent, (unsigned)turn.commitSent);
        }
        if (turn.cancel && turn.active) {
            if (s_socketConnected && !s_socket.sendTXT("{\"type\":\"cancel\"}"))
                disconnectAfterWriteFailure("cancel");
            finishSession(SendOutcome::Cancelled, nullptr, turn.epoch);
        }

        // Fail a session that can't make progress — checked EVERY iteration,
        // including while WiFi is down, so a recording made before WiFi is ready
        // (e.g. right after the camera) never hangs forever. If the socket can't
        // connect and flush the commit within the grace window, fail fast.
        turn = turnSnapshot();
        if (turn.active && turn.commitRequested && turn.commitAt != 0 && !turn.commitSent &&
            (uint32_t)(millis() - turn.commitAt) > cfg::agent::kConnectGraceMs) {
            Serial.printf("[agent] pre-commit timeout: age=%lu wifi=%d suspended=%d wl=%d wsConfigured=%d wsConnected=%d startSent=%d ringUsed=%lu sent=%lu\n",
                          (unsigned long)(millis() - turn.commitAt),
                          hal::wifi::connected() ? 1 : 0,
                          hal::wifi::suspended() ? 1 : 0,
                          (int)WiFi.status(),
                          s_socketConfigured ? 1 : 0,
                          s_socketConnected ? 1 : 0,
                          turn.startSent ? 1 : 0,
                          (unsigned long)turn.queued,
                          (unsigned long)turn.sent);
            finishSession(SendOutcome::NetworkError, "Agent connection timed out", turn.epoch);
        }

        // While WiFi is unavailable — deliberately suspended for the camera/photos
        // (which steal the radio's PSRAM), or simply not connected — tear the
        // socket down and STOP driving it. Otherwise WebSocketsClient::loop()
        // auto-reconnects on its own timer and storms start_ssl_client() with
        // "Host is unreachable", leaking fds/heap until the device crashes. An
        // active take's audio stays buffered and re-streams once WiFi returns.
        if (hal::wifi::suspended() || !hal::wifi::connected()) {
            services::agent_coordination::setWorkerPhase(coordination::NetworkCleanup);
            if (s_socketConfigured) {
                s_socket.disconnect();
                s_socketConfigured = false;
                handleSessionDrop();
            }
            services::agent_coordination::setWorkerPhase(coordination::Waiting);
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        // Microphone capture starts immediately into PSRAM. A new TLS handshake
        // must wait for upload/poller/widget cleanup to return their internal-
        // memory stacks. An already-open socket needs no new TLS memory, so it
        // keeps streaming instead of stalling behind an in-flight upload.
        // Keep cancellation and the connection deadline above this gate.
        if (!(s_socketConfigured && s_socketConnected) &&
            !services::background_network::kamiVoiceNetworkReady()) {
            services::agent_coordination::setWorkerPhase(coordination::Waiting);
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }

        // Honor backend or API-key changes — but never mid-session, so an
        // in-flight chat isn't severed. Dropping the socket + clearing the
        // configured flag makes the block below re-run configureSocket(),
        // which re-reads the endpoint and credentials for the next connection.
        const uint32_t apiKeyRevision = core::secrets::apiKeyRevision();
        if (!services::agent_coordination::isSessionActive() &&
            (s_reconfigureRequest.exchange(false) || apiKeyRevision != s_apiKeyRevision)) {
            s_apiKeyRevision = apiKeyRevision;
            services::agent_coordination::setWorkerPhase(coordination::NetworkCleanup);
            s_socket.disconnect();
            s_socketConfigured = false;
        }
        if (!s_socketConfigured) {
            services::agent_coordination::setWorkerPhase(coordination::SocketConfigure);
            configureSocket();
            s_socketConfigured = true;
        }
        services::agent_coordination::setWorkerPhase(coordination::SocketLoop);
        const uint32_t loopAt=millis();
        s_socket.loop();
        const uint32_t loopMs=millis()-loopAt;
        if(loopMs>=250) LOGI("agent-io", "up=%lu socket_ms=%lu", (unsigned long)millis(), (unsigned long)loopMs);
        sendHelloIfPending();
        if (s_socketConnected) agent_delivery::pump(sendTextRequest);
        turn = turnSnapshot();
        if (turn.active && turn.commitRequested && !turn.commitSent &&
            (uint32_t)(millis() - s_lastTransportWaitLogMs) >= 2500u) {
            s_lastTransportWaitLogMs = millis();
            Serial.printf("[agent] waiting to commit: age=%lu wifi=%d wl=%d wsConnected=%d startSent=%d ringUsed=%lu sent=%lu target=%s:%u\n",
                          (unsigned long)(millis() - turn.commitAt),
                          hal::wifi::connected() ? 1 : 0,
                          (int)WiFi.status(),
                          s_socketConnected ? 1 : 0,
                          turn.startSent ? 1 : 0,
                          (unsigned long)turn.queued,
                          (unsigned long)turn.sent,
                          s_wsHost,
                          (unsigned)s_wsPort);
        }
        if (turn.lost && turn.active) {
            finishSession(SendOutcome::NetworkError, "WebSocket disconnected during recording", turn.epoch);
        } else {
            services::agent_coordination::setWorkerPhase(coordination::AudioPump);
            const uint32_t pumpAt=millis();
            pumpAudio();
            const uint32_t pumpMs=millis()-pumpAt;
            if(pumpMs>=250) LOGI("agent-io", "up=%lu pump_ms=%lu", (unsigned long)millis(), (unsigned long)pumpMs);
            // Reply timeout (the connect-grace fast-fail runs at the top of the
            // loop so it also fires while WiFi is down).
            turn = turnSnapshot();
            if (turn.active && turn.commitRequested && turn.commitAt != 0 && turn.commitSent &&
                (uint32_t)(millis() - turn.commitAt) > cfg::agent::kResponseTimeoutMs) {
                finishSession(SendOutcome::NetworkError, "Agent timed out", turn.epoch);
            }
        }
        if (services::agent_coordination::isSessionActive()) services::notifyUserActivity();
        services::agent_coordination::setWorkerPhase(coordination::Waiting);
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void beginStreamingRecording() {
    const auto epoch=s_replies.reset();
    s_uiCancelled=s_uiRejected=false;
    s_uiCancelTag=0;
    s_sendResult = {};
    s_sendDone = false;
    s_replyReady = false;
    portENTER_CRITICAL(&s_audioMux);
    resetAudioRingLocked();
    s_turnEpoch=epoch;
    s_replyReceived=false;
    s_sendCancel = false;
    s_sendCancelNav = 0;
    s_sentSamples = 0;
    s_startSent = false;
    s_listening = false;
    s_commitSent = false;
    s_commitRequested = false;
    s_commitAtMs = 0;
    s_transportLost = false;
    s_audioAttached = true;
    services::agent_coordination::setSessionActive(true);
    portEXIT_CRITICAL(&s_audioMux);
    LOGI("agent-turn", "e=%lu up=%lu begin", (unsigned long)epoch, (unsigned long)millis());
}

void abandonActiveSession() {
    portENTER_CRITICAL(&s_audioMux);
    resetSessionFlags();
    s_turnEpoch=0;
    resetAudioRingLocked();
    portEXIT_CRITICAL(&s_audioMux);
    s_replies.reset();
    s_replyReady=s_sendDone=false;
    s_uiCancelled=s_uiRejected=false;
}
}  // namespace

// ─── Public async-send API ───────────────────────────────────────────────────

bool prepare(const VoiceCallbacks& callbacks, bool reconnectIfOffline) {
    if (reconnectIfOffline && !hal::wifi::connected()) {
        Serial.println("[agent] enter(): not connected -> hal::wifi::connect()");
        (void)hal::wifi::connect();
    }
    s_callbacks.store(&callbacks);
    if (s_sendHandle) return true;
    s_sendHandle = hal::tasks::spawn(hal::tasks::Id::KamiSend, kamiSendTask);
    if (s_sendHandle) services::agent_coordination::workerStarted();
    return s_sendHandle != nullptr;
}

static void reconfigure() {
    // Just raise the flag — kamiSendTask owns s_socket and performs the actual
    // disconnect/reconnect on its own thread (WebSocketsClient isn't safe to
    // touch concurrently with its loop()). Before worker startup the request
    // stays pending; initial configuration takes a fresh backendConfig() snapshot.
    s_reconfigureRequest.store(true);
}

void selectBackendMode(bool dev) {
    if (core::secrets::isDevMode() == dev) return;
    if (core::secrets::setDevMode(dev)) reconfigure();
}

bool setBackendUrl(const char* url) {
    const bool saved = core::secrets::setBackendUrl(url);
    if (saved) reconfigure();
    return saved;
}

void begin() { beginStreamingRecording(); }
void detachAudio() {
    // Finish any in-flight ring copy before its owner frees the slab. Keep heap
    // release outside the critical section; subsequent copies see detached.
    portENTER_CRITICAL(&s_audioMux);
    s_audioAttached = false;
    s_ringRead = s_ringWrite = s_ringUsed = 0;
    portEXIT_CRITICAL(&s_audioMux);
}
void abandon() { abandonActiveSession(); }
bool appendAudio(const int16_t* samples, uint32_t count, void* user) {
    return streamMicSamples(samples,count,user);
}
void commit() {
    s_replyReady=false;
    const uint32_t now = millis();
    portENTER_CRITICAL(&s_audioMux);
    s_commitRequested=true;
    s_commitAtMs=now;
    portEXIT_CRITICAL(&s_audioMux);
}
void cancel(uint8_t tag) {
    s_uiCancelled=true;
    s_uiCancelTag=tag;
    portENTER_CRITICAL(&s_audioMux);
    const bool active = services::agent_coordination::isSessionActive();
    if (active) { s_sendCancelNav=tag; s_sendCancel=true; }
    portEXIT_CRITICAL(&s_audioMux);
    if (!active) {
        // A terminal result may already be queued but not consumed by the UI.
        // Discard it and run cleanup here; it must not resurrect the cancelled view.
        s_replies.reset();
        const auto* callbacks=s_callbacks.load();
        if (callbacks) callbacks->finish(Outcome::Cancelled,nullptr,0);
        s_sendResult.outcome=Outcome::Cancelled; s_sendResult.cancelTag=tag; s_sendResult.pendingTasks=0;
        s_replyReady=false; s_sendDone=true;
        return;
    }
}
void pollReplies() {
    const auto* callbacks=s_callbacks.load();
    if (!callbacks) return;
    ReplyMailbox::Event event;
    // Bound callback work per UI pass; queued completion waits behind all text.
    for (unsigned n=0; n<32 && s_replies.pop(event); ++n) {
        if (s_uiCancelled || s_uiRejected) continue;
        if (event.type()==ReplyMailbox::Type::Metadata) {
            if(callbacks->event) callbacks->event(event.text());
        }
        else if (event.type()==ReplyMailbox::Type::Discard) {
            if (callbacks->discard) callbacks->discard(atoi(event.text()));
            s_replyReady=callbacks->hasReply();
        }
        else if (event.type()==ReplyMailbox::Type::Status) callbacks->status(event.text());
        else if (!callbacks->message(event.text(),event.type()==ReplyMailbox::Type::Token)) {
            s_uiRejected=true;
            portENTER_CRITICAL(&s_audioMux);
            s_sendCancel=true;
            portEXIT_CRITICAL(&s_audioMux);
        } else s_replyReady=callbacks->hasReply();
    }
    ReplyMailbox::Completion completion;
    if (!s_replies.takeCompletion(completion)) return;
    if (s_uiCancelled) { completion.outcome=Outcome::Cancelled; completion.cancelTag=s_uiCancelTag; }
    else if (s_uiRejected) {
        completion.outcome=Outcome::NetworkError;
        snprintf(completion.error,sizeof(completion.error),"%s","Reply buffer full");
    }
    s_sendResult.outcome=callbacks->finish(completion.outcome,completion.error,completion.sentSamples);
    s_sendResult.cancelTag=completion.cancelTag;
    s_sendResult.pendingTasks=completion.outcome==Outcome::Ok ? completion.pendingTasks : 0;
    s_replyReady=false;
    s_sendDone=true;
    const auto stats=s_replies.stats();
    LOGI("agent-ui", "up=%lu end=%u peak=%u q=%u events=%u", (unsigned long)millis(),
         (unsigned)s_sendResult.outcome, (unsigned)stats.peakBytes, (unsigned)stats.bytes, (unsigned)stats.events);
    Serial.printf("[agent-reply] drained peak=%u queued=%u events=%u\n",
                  (unsigned)stats.peakBytes,(unsigned)stats.bytes,(unsigned)stats.events);
    core::memory::logSnapshot("agent.reply.drained");
}
Status status() {
    const uint32_t now = millis();
    portENTER_CRITICAL(&s_audioMux);
    const Status result{socketReadyForAudioLocked(now), s_sendDone, s_commitRequested,
        s_commitSent, s_replyReady, s_streamOverflow, s_streamDropped};
    portEXIT_CRITICAL(&s_audioMux);
    return result;
}
ReplyQueueStats replyQueueStats() {
    const auto stats=s_replies.stats();
    return {(uint32_t)stats.bytes,(uint32_t)stats.peakBytes,(uint32_t)stats.events};
}
Result result() { return s_sendResult; }
void clearDone() { s_sendDone=false; }
void clearReplyReady() { s_replyReady=false; }
const char* outcomeName(Outcome outcome) {
    switch (outcome) {
        case Outcome::Ok: return "ok";
        case Outcome::HttpError: return "http_error";
        case Outcome::NetworkError: return "network_error";
        case Outcome::Cancelled: return "cancelled";
    }
    return "unknown";
}

} }
