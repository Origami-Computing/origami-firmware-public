#include "services/net_poll.h"
#include "services/account_info.h"
#include "net_poll_lifecycle.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <WiFiClientSecure.h>
#include "services/tls_trust.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "services/automation.h"
#include "services/device_fs.h"
#include "core/device_id.h"
#include "services/eyes_asset.h"
#include "core/menu_config.h"
#include "core/secrets.h"
#include "core/telemetry_queue.h"
#include "core/telemetry.h"
#include "core/watchdog.h"
#include "services/background_network.h"
#include "hal/hal_tasks.h"
#include "hal/hal_wifi.h"

namespace services {
namespace net_poll {

namespace {

// Loop cadence. Short enough that "due" jobs fire promptly, long enough that an
// idle device barely costs anything. All real waiting is this vTaskDelay; the
// blocking HTTP happens inside the jobs themselves.
constexpr uint32_t kTickMs = 1000;

static portMUX_TYPE s_healthMux = portMUX_INITIALIZER_UNLOCKED;
static char s_healthJson[core::telemetry_queue::kItemJsonCap] = {};
static uint32_t s_healthSequence = 0;
static bool s_healthPending = false;
static const char* s_phase="not_started";
static uint32_t s_phaseSince=0;
void phase(const char* name) {
  portENTER_CRITICAL(&s_healthMux);
  s_phase=name;s_phaseSince=millis();
  portEXIT_CRITICAL(&s_healthMux);
}

// Returns true once `intervalMs` has elapsed since `*lastMs`, and arms the next
// window. `*lastMs == 0` (the initial state) fires immediately on first tick.
bool due(uint32_t now, uint32_t* lastMs, uint32_t intervalMs) {
  if (intervalMs == 0) return false;  // 0 == disabled
  if (*lastMs != 0 && (uint32_t)(now - *lastMs) < intervalMs) return false;
  *lastMs = now;
  return true;
}

// First-boot registration: exchange the per-user X-Api-Key + stable device_id
// for a per-device bearer token, persisted in NVS. Idempotent server-side
// (upsert keyed on device_id), so a re-register after factoryReset just returns
// the same row. Returns true once a token is held (already or newly).
bool ensureRegistered() {
  const auto request = core::secrets::backendRequest();
  if (request.token[0]) return true;        // already registered
  static uint32_t lastAttempt = 0;
  if (!due(millis(), &lastAttempt, 15000u)) return false;
  const char* apiKey = request.backend.apiKey;
  if (!apiKey || !apiKey[0]) return false;                 // no user key yet → can't register

  WiFiClientSecure secClient;
  WiFiClient       plainClient;
  String           url = String(request.backend.url) + String(DEVICE_REGISTER_PATH);
  const bool       https = url.startsWith("https://");
  if (https) {
    services::tls::useRootBundle(secClient);
    secClient.setHandshakeTimeout(8); // seconds — cap TLS handshake under TWDT
  }
  WiFiClient& wc = https ? static_cast<WiFiClient&>(secClient)
                         : static_cast<WiFiClient&>(plainClient);

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(wc, url)) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Api-Key", apiKey);
  http.addHeader("X-Device-Id", core::device::id());

  JsonDocument body;
  body["device_id"] = core::device::id();
  body["fw"]        = core::device::firmwareVersion();
  String payload;
  serializeJson(body, payload);

  const int code = http.POST(payload);
  if (code != 200 && code != 201) {
    http.end();
    LOGW("net_poll", "Registration failed HTTP %d", code);
    return false;
  }
  const String resp = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, resp)) return false;
  const char* token = doc["token"] | "";
  if (!token[0]) return false;
  if (!core::secrets::setDeviceTokenFor(request, token)) return false;
  LOGI("net_poll", "Registered device %s", core::device::id());
  return true;
}

// POST a JSON body to {backend}{path} with device auth. Prefers the per-device
// bearer token; falls back to the shared X-Api-Key until the device registers.
// Returns the HTTP status (or -1 on transport failure).
int postJson(const char* path, const String& body) {
  const auto request = core::secrets::backendRequest();
  WiFiClientSecure secClient;
  WiFiClient       plainClient;
  String           url = String(request.backend.url) + String(path);
  const bool       https = url.startsWith("https://");
  if (https) {
    services::tls::useRootBundle(secClient);
    secClient.setHandshakeTimeout(8); // seconds — cap TLS handshake under TWDT
  }
  WiFiClient& wc = https ? static_cast<WiFiClient&>(secClient)
                         : static_cast<WiFiClient&>(plainClient);

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(wc, url)) return -1;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-Device-Id", core::device::id());
  const char* token = request.token;
  if (token[0]) {
    http.addHeader("Authorization", String("Bearer ") + token);
  } else {
    http.addHeader("X-Api-Key", request.backend.apiKey);
  }
  const int code = http.POST(const_cast<String&>(body));
  if (code < 200 || code >= 300) Serial.printf("[net_poll] POST %s HTTP %d bytes=%u heap=%u\n", path, code, (unsigned)body.length(), (unsigned)ESP.getFreeHeap());
  http.end();
  if (code == 401) core::secrets::setDeviceTokenFor(request, "");
  return code;
}

bool ok2xx(int code) { return code >= 200 && code < 300; }
// Drop poison-pill batches the server rejects as malformed so we do not loop forever.
// Preserve events on 401 (re-register), throttling and transient failures.
bool retryable(int code) { return code < 0 || code == 0 || code >= 500 || code == 401 || code == 408 || code == 429; }

// Build {device_id, fw, logs:[...]} from a run of KIND_LOG items.
String buildLogsBody(const core::telemetry_queue::Item* items, int n) {
  JsonDocument doc;
  doc["device_id"] = core::device::id();
  doc["fw"]        = core::device::firmwareVersion();
  JsonArray logs = doc["logs"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    JsonDocument entry;
    if (deserializeJson(entry, items[i].json) == DeserializationError::Ok) {
      logs.add(entry);
    }
  }
  String out;
  serializeJson(doc, out);
  return out;
}

// Health is a latest-value mailbox, independent of the diagnostic FIFO. A
// successful in-flight send must not clear a newer sample published by the UI.
void sendLatestHealth() {
  char json[core::telemetry_queue::kItemJsonCap];
  uint32_t sequence;
  portENTER_CRITICAL(&s_healthMux);
  const bool pending = s_healthPending;
  sequence = s_healthSequence;
  memcpy(json, s_healthJson, sizeof(json));
  portEXIT_CRITICAL(&s_healthMux);
  if (!pending) return;
  if (!ok2xx(postJson(TELEMETRY_HEARTBEAT_PATH, String(json)))) return;
  portENTER_CRITICAL(&s_healthMux);
  if (sequence == s_healthSequence) s_healthPending = false;
  portEXIT_CRITICAL(&s_healthMux);
}

// Drain the telemetry queue: ship logs in batches, crashes/heartbeats singly.
// Items are acked as a contiguous successful prefix (matches ack() semantics);
// the first failure stops the pass so the rest retry next tick.
void drainTelemetry() {
  using namespace core::telemetry_queue;
  if (!hasPending()) return;
  // Keep ~3 KB of batch scratch out of the internal task stack. The worker
  // must be restartable after audio frees it into a fragmented internal heap.
  Item* batch = static_cast<Item*>(heap_caps_malloc(sizeof(Item) * TELEMETRY_BATCH_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!batch) return;
  const int n = snapshot(batch, TELEMETRY_BATCH_MAX);
  int sent = 0;
  int i = 0;
  while (i < n && !stopRequested() && !background_network::PeriodicWork::hasWaitingPeer()) {
    core::watchdog::feed();  // each POST can block seconds; keep the WDT happy between them
    if (batch[i].kind == KIND_LOG || batch[i].kind == KIND_VERBOSE) {
      int j = i;
      size_t payloadBytes = 0;
      // Keep each JSON POST below ~1 KB. Larger TLS writes failed under the
      // same fragmented lwIP memory pressure seen during full-card uploads.
      while (j < n && (batch[j].kind == KIND_LOG || batch[j].kind == KIND_VERBOSE)) {
        const size_t bytes = strlen(batch[j].json) + 1;
        if (j > i && payloadBytes + bytes > 800) break;
        payloadBytes += bytes;
        j++;
      }
      const int code = postJson(TELEMETRY_LOGS_PATH, buildLogsBody(&batch[i], j - i));
      if (ok2xx(code)) { sent += (j - i); i = j; continue; }
      if (!retryable(code)) { sent += (j - i); i = j; continue; }  // drop poison batch
      break;
    }
    const char* path = (batch[i].kind == KIND_CRASH) ? TELEMETRY_CRASH_PATH
                                                      : TELEMETRY_HEARTBEAT_PATH;
    const int code = postJson(path, String(batch[i].json));
    if (ok2xx(code) || !retryable(code)) { sent++; i++; continue; }
    break;
  }
  if (sent > 0) {
    ack(sent);
    const auto stats = core::telemetry_queue::stats();
    // Keep transport bookkeeping local: forwarding this status queues another
    // upload after every successful drain, even when the device is idle.
    Serial.printf("[telemetry] sent=%d queued=%lu drop_v=%lu drop_d=%lu\n", sent,
         (unsigned long)stats.queued, (unsigned long)stats.droppedVerbose,
         (unsigned long)stats.droppedDurable);
  }
  heap_caps_free(batch);
}

} // namespace

void lifecycle::run(void*) {
  awaitPublication();
  uint32_t lastAutomationPoll = 0;
  uint32_t lastHealthAttempt = 0;
  uint32_t lastTelemetryDrain = 0;
  uint32_t lastFsPoll = 0;

  core::watchdog::subscribe();  // freeze detection for the periodic-HTTP worker

  do {
    for (;;) {
      if (stopRequested()) break;
      // Feed before AND after each blocking HTTP op below: a single iteration can
      // chain register + poll + a drain batch, any of which blocks for seconds.
      // Feeding around them keeps a healthy task alive while still catching a true
      // hang (a wedged TLS read never returns and trips the watchdog).
      core::watchdog::feed();
      const uint32_t now = millis();

      // All jobs require a live link. Skip cleanly when offline; the SD-persisted
      // telemetry queue preserves work until WiFi returns.
      if (hal::wifi::connected()) {
        background_network::PeriodicWork periodic;
        if (!periodic.tryEnter()) {
          phase("periodic_wait");
          ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kTickMs));
          continue; // No transport acquired; normal stop check and watchdog feed next tick.
        }
        auto yieldToWaitingRequest = [&]() {
          if (!periodic.yieldIfContended()) return false;
          phase("periodic_yield");
          ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kTickMs));
          return true;
        };
        // ── first-boot registration ──────────────────────────────────────────
        // Cheap no-op once a token is held. Telemetry POSTs use the token when
        // present and fall back to X-Api-Key until then.
        phase("registration");
        const bool registered = ensureRegistered();
        core::watchdog::feed();

        if (stopRequested()) break;
        if (yieldToWaitingRequest()) continue;

        // Health goes before logs, file transfers and less frequent HTTP jobs.
        if (registered && due(now, &lastHealthAttempt, 5000u)) {
          phase("health");
          sendLatestHealth();
          core::watchdog::feed();
        }

        if (stopRequested()) break;
        if (yieldToWaitingRequest()) continue;

        // Repair the canonical eye sprite once when SD provisioning missed it.
        // The download is streamed and verified; healthy cards make this a no-op.
        phase("eyes_asset");
        services::eyes_asset::service();
        core::watchdog::feed();

        if (stopRequested()) break;
        if (yieldToWaitingRequest()) continue;

        phase("account");
        services::account_info::poll();
        core::watchdog::feed();
        if (stopRequested()) break;
        if (yieldToWaitingRequest()) continue;

        // ── automations GET ──────────────────────────────────────────────────
        // Moved off ui_task: this is the blocking HTTPS GET that used to stall the
        // display pipeline every poll interval. Parsed entries are handed back to
        // ui_task via the automation inbox (drainPolledInbox()).
        if (due(now, &lastAutomationPoll, AUTOMATION_POLL_INTERVAL_MS)) {
          phase("automations");
          services::automation::pollTransportNow();
          core::watchdog::feed();
        }

        if (stopRequested()) break;
        if (yieldToWaitingRequest()) continue;

        // ── telemetry drain → POST /api/device/{logs,crashes,heartbeat} ───────
        if (registered && due(now, &lastTelemetryDrain, TELEMETRY_DRAIN_INTERVAL_MS)) {
          phase("telemetry");
          drainTelemetry();  // feeds internally after each POST
        }

        if (stopRequested()) break;
        if (yieldToWaitingRequest()) continue;

        // ── device file sync → GET /api/device/fs/ops, execute queued ops ─────
        // Drains web-enqueued file operations (browse manifest, push/pull, etc).
        // Long transfers poll stopRequested() so a voice/music suspend (which
        // stops this task) aborts them promptly. Excludes /upload_queue.
        if (registered && due(now, &lastFsPoll, DEVICE_FS_POLL_INTERVAL_MS)) {
          phase("device_fs");
          services::device_fs::poll();
          core::watchdog::feed();
        }

        // ── later phases slot in here, each gated by its own due() timer ───────
        //   OTA version check    → push OTA app to ui_task
      }

      // stop() notifies us: do not sleep through a foreground audio request.
      phase("idle");
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kTickMs));
    }

  } while (!claimRetirement()); // Resume may cancel stop before retirement is claimed.
  phase("retiring");
  core::watchdog::unsubscribe();
  hal::tasks::recordCurrentStack(hal::tasks::Id::NetPoll);
  publishQuiescence();
  hal::tasks::deleteCurrent(hal::tasks::Id::NetPoll);
}

void printStatus(Print& out) {
  portENTER_CRITICAL(&s_healthMux);
  const char* current=s_phase;const uint32_t since=s_phaseSince;
  portEXIT_CRITICAL(&s_healthMux);
  out.printf("[net-status] phase=%s age_ms=%lu running=%u stop=%u\n",current,
             (unsigned long)(millis()-since),running(),stopRequested());
}

void setHealthSnapshot(const char* json) {
  portENTER_CRITICAL(&s_healthMux);
  strlcpy(s_healthJson, json ? json : "", sizeof(s_healthJson));
  ++s_healthSequence;
  s_healthPending = true;
  portEXIT_CRITICAL(&s_healthMux);
}

}  // namespace net_poll
}  // namespace services
