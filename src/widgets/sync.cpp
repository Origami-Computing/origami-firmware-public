/**
 * @file sync.cpp
 * @brief Pull Lua device widgets from the kami backend onto the SD card.
 *
 * Counterpart of the server's /api/device-widgets endpoints (kami2 branch
 * feature-device-widgets). v1 is deliberately simple: re-fetch every widget
 * on each sync rather than diffing updated_at — the manifest is tiny and a
 * full sync of a handful of widgets is a couple of seconds.
 */

#include "widgets/sync.h"
#include "widgets/path_validation.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include "services/bounded_http.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "services/tls_trust.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <atomic>

#include "apps/agent/agent_flow.h"
#include "services/eyes_asset.h"
#include "services/muni_transit.h"
#include "services/agent_coordination.h"
#include "services/background_network.h"
#include "core/secrets.h"
#include "hal/hal_sd.h"
#include "hal/hal_tasks.h"

namespace widgets {
namespace sync {

namespace {

constexpr char kManifestPath[] = "/api/device-widgets";
constexpr size_t kMaxManifestBytes = 16u * 1024u;
constexpr size_t kMaxWidgets = 32;
constexpr size_t kMaxScriptBytes = 32u * 1024u;  // matches lua_app load cap
constexpr size_t kMaxDataBytes   = 8u * 1024u;   // matches lua_app data cap

// How often the poll task wakes up to check for new/updated widgets.
constexpr uint32_t kPollIntervalMs = 30000u;

// Cross-core notification: written by widget_poll (Core 0), read by ui_task.
// Atomic publication/consumption; no lock is held across I/O or task waits.
static std::atomic<bool> s_syncNotify{false};
// Accessed only by widget_poll, including its cancellation checkpoints.
static services::background_network::PeriodicWork s_periodicWork;
static std::atomic<TaskHandle_t> s_pollTask{nullptr};
static std::atomic<bool> s_pauseRequested{false};
static std::atomic<bool> s_pauseAcknowledged{false};

bool pollPausePending() {
    return s_pauseRequested && xTaskGetCurrentTaskHandle() == s_pollTask;
}

// Called only by widget_poll after it has released all HTTP/TLS and SD state.
// Returning true tells the caller to abandon the current poll cycle after the
// pause is released, rather than resuming a half-completed synchronization.
bool pauseAtIdleCheckpoint() {
    if (!pollPausePending()) return false;
    s_periodicWork.release();
    s_pauseAcknowledged = true;
    while (s_pauseRequested) {
        // Re-acknowledge if resume and the next PTT arrive before this task
        // wakes: resume clears the old acknowledgement to avoid stale state.
        s_pauseAcknowledged = true;
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
    s_pauseAcknowledged = false;
    return true;
}

void setErr(char* errOut, size_t errLen, const char* msg) {
    if (errOut && errLen > 0) snprintf(errOut, errLen, "%s", msg);
}

// djb2 hash over a byte buffer — fast and good enough to detect manifest changes.
static uint32_t djb2(const char* s, size_t len) {
    uint32_t h = 5381u;
    for (size_t i = 0; i < len; ++i) h = h * 33u ^ (uint8_t)s[i];
    return h;
}

// Fetch the raw manifest payload and return its hash, or 0 on failure.
static uint32_t fetchManifestHash() {
    if (WiFi.status() != WL_CONNECTED) return 0;
    const auto config = core::secrets::backendConfig();
    WiFiClient plain;
    WiFiClientSecure secure;
    services::tls::useRootBundle(secure);
    secure.setHandshakeTimeout(8);
    WiFiClient& transport = strncmp(config.url, "https://", 8) == 0
                                ? static_cast<WiFiClient&>(secure) : plain;
    HTTPClient http;
    services::bounded_http::configure(http);
    http.setConnectTimeout(4000);
    http.begin(transport, String(config.url) + kManifestPath);
    http.addHeader("X-Api-Key", config.apiKey);
    const int code = http.GET();
    if (code != 200) { http.end(); return 0; }
    String payload;
    const bool bodyOk = services::bounded_http::read(http, kMaxManifestBytes, payload);
    http.end();
    if (!bodyOk || payload.isEmpty()) return 0;
    return djb2(payload.c_str(), payload.length());
}

// Certificate verification is CPU-bound (seconds for a P-384 chain such as
// staging's) and this worker shares core 0 with net_poll and the idle task.
// Block briefly between requests so neither misses its task-watchdog deadline.
static void yieldBetweenRequests() { vTaskDelay(pdMS_TO_TICKS(50)); }

// Fetch one widget's main.lua (+ optional data.json) and write it to
// /widgets/<slug>/. Returns true on success. Shared by the full sync and the
// targeted syncOne. The caller owns `transport`; requests keep it alive so a
// sync pays for one TLS handshake, not one per request.
static bool fetchAndWriteWidget(const core::secrets::BackendConfig& config, WiFiClient& transport,
                                const char* id, const char* slug) {
    if (!validPathSegment(id, 64) || !validPathSegment(slug, 31)) return false;
    const String base(config.url);
    HTTPClient get;
    services::bounded_http::configure(get);
    get.setReuse(false);
    get.setConnectTimeout(4000);
    get.begin(transport, base + kManifestPath + "/" + id + "/main.lua");
    get.addHeader("X-Api-Key", config.apiKey);
    const int c = get.GET();
    if (c != 200) {
        get.end();
        Serial.printf("[widget-sync] %s: HTTP %d, skipped\n", slug, c);
        return false;
    }
    String lua;
    const bool scriptOk = services::bounded_http::read(get, kMaxScriptBytes, lua);
    get.end();
    yieldBetweenRequests();
    if (pollPausePending()) return false;
    if (!scriptOk || lua.length() == 0 || lua.length() > kMaxScriptBytes) {
        Serial.printf("[widget-sync] %s: bad size %u, skipped\n", slug, (unsigned)lua.length());
        return false;
    }

    char dir[64];
    char path[80];
    snprintf(dir, sizeof(dir), "/widgets/%s", slug);
    snprintf(path, sizeof(path), "/widgets/%s/main.lua", slug);
    if (!hal::sd::ensureDir("/widgets") || !hal::sd::ensureDir(dir)) {
        Serial.printf("[widget-sync] %s: mkdir failed, skipped\n", slug);
        return false;
    }
    if (!hal::sd::writeFile(path, (const uint8_t*)lua.c_str(), lua.length())) {
        Serial.printf("[widget-sync] %s: SD write failed, skipped\n", slug);
        return false;
    }
    Serial.printf("[widget-sync] wrote %s (%u B)\n", path, (unsigned)lua.length());

    // Optional data snapshot, exposed to the script as kami.data.
    HTTPClient dataGet;
    services::bounded_http::configure(dataGet);
    dataGet.setReuse(false);
    dataGet.setConnectTimeout(4000);
    dataGet.begin(transport, base + kManifestPath + "/" + id + "/data.json");
    dataGet.addHeader("X-Api-Key", config.apiKey);
    if (dataGet.GET() == 200) {
        String json;
        const bool dataOk = services::bounded_http::read(dataGet, kMaxDataBytes, json);
        if (dataOk && json.length() > 0 && json.length() <= kMaxDataBytes) {
            char dpath[80];
            snprintf(dpath, sizeof(dpath), "/widgets/%s/data.json", slug);
            if (hal::sd::writeFile(dpath, (const uint8_t*)json.c_str(), json.length())) {
                Serial.printf("[widget-sync] wrote %s (%u B)\n", dpath, (unsigned)json.length());
            }
        }
    }
    dataGet.end();
    yieldBetweenRequests();
    return true;
}

static void pollTask(void*) {
    s_pollTask = xTaskGetCurrentTaskHandle();
    uint32_t lastHash = 0;
    bool loggedAgentWait = false;
    bool retryAdmission = false;
    for (;;) {
        s_periodicWork.release();
        if (pauseAtIdleCheckpoint()) continue;
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((retryAdmission || services::muni::isActive()) ? 500u : kPollIntervalMs));
        retryAdmission = false;
        if (pauseAtIdleCheckpoint()) continue;
        if (WiFi.status() != WL_CONNECTED) continue;
        const bool liveTransit = services::muni::isActive();
        if (liveTransit && !services::muni::pollDue()) continue;
        const bool agentReady = apps::agent::flow::isTransportReady();
        // Bulk backend sync waits for the primary socket. Public transit must
        // also work when that backend is unavailable, but cannot race its TLS
        // reconnect. Borrow the existing acknowledged pause only while offline.
        if (!agentReady && !liveTransit) {
            if (!loggedAgentWait) {
                Serial.println("[widget-poll] waiting for agent transport before HTTPS sync");
                loggedAgentWait = true;
            }
            continue;
        }
        loggedAgentWait = false;
        if (!s_periodicWork.tryEnter()) { retryAdmission = true; continue; }
        services::agent_coordination::TransportPause reconnectPause;
        if (!agentReady) {
            if (services::agent_coordination::isSessionActive() ||
                !reconnectPause.acquire(1000)) { retryAdmission = true; continue; }
            if (pollPausePending() || services::agent_coordination::isSessionActive()) continue;
        }
        services::muni::poll(pollPausePending);
        reconnectPause.release();
        if (pauseAtIdleCheckpoint()) continue;
        // Foreground live data takes this worker's slot. Defer bulk automatic
        // script downloads until that subscription closes; targeted launches
        // retain their existing foreground sync path.
        if (services::muni::isActive()) continue;
        if (!hal::sd::mounted()) continue;
        // Net polling is disabled in some hardware-diagnostic builds. Offer
        // the same one-shot recovery opportunity from this existing TLS-sized
        // worker once the primary agent transport has settled.
        services::eyes_asset::service();
        if (pauseAtIdleCheckpoint()) continue;
        const uint32_t h = fetchManifestHash();
        if (pauseAtIdleCheckpoint()) continue;
        if (h == 0 || h == lastHash) continue;
        Serial.printf("[widget-poll] manifest changed (hash %08lx → %08lx), syncing\n",
                      (unsigned long)lastHash, (unsigned long)h);
        char err[48] = "";
        const int n = syncFromBackend(err, sizeof(err));
        if (pauseAtIdleCheckpoint()) continue;
        if (n < 0) {
            Serial.printf("[widget-poll] sync failed: %s\n", err);
        } else {
            Serial.printf("[widget-poll] synced %d widget(s)\n", n);
            lastHash = h;
            s_syncNotify = true;
        }
    }
}

}  // namespace

int syncFromBackend(char* errOut, size_t errLen) {
    if (WiFi.status() != WL_CONNECTED) {
        setErr(errOut, errLen, "wifi not connected");
        return -1;
    }
    if (!hal::sd::mounted()) {
        setErr(errOut, errLen, "no SD card");
        return -1;
    }

    const auto config = core::secrets::backendConfig();
    WiFiClient plain;
    WiFiClientSecure secure;
    services::tls::useRootBundle(secure);
    secure.setHandshakeTimeout(8);
    WiFiClient& transport = strncmp(config.url, "https://", 8) == 0
                                ? static_cast<WiFiClient&>(secure) : plain;
    HTTPClient http;
    services::bounded_http::configure(http);
    http.setReuse(false);
    http.setConnectTimeout(4000);
    String base = String(config.url);
    http.begin(transport, base + kManifestPath);
    http.addHeader("X-Api-Key", config.apiKey);
    int code = http.GET();
    if (code != 200) {
        http.end();
        if (errOut) snprintf(errOut, errLen, "manifest HTTP %d", code);
        return -1;
    }
    String payload;
    const bool bodyOk = services::bounded_http::read(http, kMaxManifestBytes, payload);
    http.end();
    if (pollPausePending()) {
        setErr(errOut, errLen, "paused");
        return -1;
    }

    JsonDocument doc;
    if (!bodyOk || deserializeJson(doc, payload) || !doc.is<JsonArray>() || doc.size() > kMaxWidgets) {
        setErr(errOut, errLen, "bad manifest JSON");
        return -1;
    }

    int written = 0;
    for (JsonObject w : doc.as<JsonArray>()) {
        if (xTaskGetCurrentTaskHandle() == s_pollTask && services::muni::isActive()) {
            setErr(errOut, errLen, "deferred for live data");
            return -1; // Retry the manifest later; do not accept a partially synced hash.
        }
        if (pollPausePending()) {
            setErr(errOut, errLen, "paused");
            return -1;
        }
        const char* id = w["id"];
        const char* slug = w["slug"];
        if (!id || !slug || !slug[0]) continue;
        if (fetchAndWriteWidget(config, transport, id, slug)) written++;
    }
    return written;
}

int syncOne(const char* id, const char* slug, char* errOut, size_t errLen) {
    if (!id || !slug || !slug[0]) { setErr(errOut, errLen, "bad id/slug"); return -1; }
    if (WiFi.status() != WL_CONNECTED) { setErr(errOut, errLen, "wifi not connected"); return -1; }
    if (!hal::sd::mounted()) { setErr(errOut, errLen, "no SD card"); return -1; }
    const auto config = core::secrets::backendConfig();
    WiFiClient plain;
    WiFiClientSecure secure;
    services::tls::useRootBundle(secure);
    secure.setHandshakeTimeout(8);
    WiFiClient& transport = strncmp(config.url, "https://", 8) == 0
                                ? static_cast<WiFiClient&>(secure) : plain;
    if (!fetchAndWriteWidget(config, transport, id, slug)) { setErr(errOut, errLen, "fetch/write failed"); return -1; }
    return 0;
}

void startPolling() {
    if (s_pollTask) return;
    (void)hal::tasks::spawn(hal::tasks::Id::WidgetPoll, pollTask);
}

void requestPoll() {
    TaskHandle_t task = s_pollTask;
    if (task) xTaskNotifyGive(task);
}

void requestPollingPause() {
    if (s_pauseRequested) return;
    s_pauseRequested = true;

    TaskHandle_t task = s_pollTask;
    if (task) xTaskNotifyGive(task);
}

bool pollingPaused() { return !s_pollTask || (s_pauseRequested && s_pauseAcknowledged); }

bool suspendPolling(uint32_t timeoutMs) {
    requestPollingPause();
    TaskHandle_t task = s_pollTask;
    if (!task) return true;
    xTaskNotifyGive(task);

    const uint32_t startedMs = millis();
    while (!s_pauseAcknowledged && (uint32_t)(millis() - startedMs) < timeoutMs) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_pauseAcknowledged) {
        Serial.printf("[widget-poll] paused in %lu ms\n",
                      (unsigned long)(millis() - startedMs));
        return true;
    }

    s_pauseRequested = false;
    xTaskNotifyGive(task);
    Serial.printf("[widget-poll] pause timed out after %lu ms\n",
                  (unsigned long)(millis() - startedMs));
    return false;
}

void resumePolling() {
    if (!s_pauseRequested) return;
    s_pauseRequested = false;
    TaskHandle_t task = s_pollTask;
    if (task) xTaskNotifyGive(task);
    Serial.println("[widget-poll] resumed");
}

bool takeSyncNotification() {
    return s_syncNotify.exchange(false);
}

}  // namespace sync
}  // namespace widgets
