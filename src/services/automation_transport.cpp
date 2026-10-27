#include "services/bounded_http.h"
#include "services/automation.h"
#include "services/recipe_alert_json.h"
#include "core/menu_config.h"
#include "core/secrets.h"
#include "core/release_policy.h"
#include "hal/hal_wifi.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "services/tls_trust.h"
#include <ctype.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <stdlib.h>
#include <string.h>

namespace services {
namespace automation {

namespace {

WebServer* s_automation_http = nullptr;

// A one-slot mailbox carries COMPLETE snapshots, including an empty list.
// Created on the UI thread before net_poll starts; only the producer overwrites.
QueueHandle_t s_inbox = nullptr;

void handleAutomationHttpPost() {
  if (!s_automation_http) return;
  WebServer& server = *s_automation_http;
  const auto config = core::secrets::backendConfig();
  if (!config.apiKey[0] || !server.hasHeader("X-Api-Key") ||
      server.header("X-Api-Key") != String(config.apiKey)) {
    server.send(401, "application/json", "{\"error\":\"unauthorized\"}");
    return;
  }

  String body = server.arg("plain");
  if (body.length() == 0) {
    body = server.arg("body");
  }

  JsonDocument doc;
  if (deserializeJson(doc, body)) {
    server.send(400, "application/json", "{\"error\":\"bad_json\"}");
    return;
  }

  JsonObjectConst o = doc.as<JsonObjectConst>();

  // Action-based commands (e.g. {"action":"ota"}) are handled before scheduled alerts.
  const char* action = o["action"] | "";
  if (action && action[0]) {
    if (strcmp(action, "ota") == 0) {
      setOtaPending();
      server.send(200, "application/json", "{\"ok\":true}");
      return;
    }
    server.send(400, "application/json", "{\"error\":\"unknown_action\"}");
    return;
  }

  services::recipe_alerts::Entry entry;
  if (!services::recipe_alerts::parseEntry(o,entry)) {
    server.send(400, "application/json", "{\"error\":\"bad_fields\"}");
    return;
  }
  if (!ingestRecipeAlert(entry)) {
    server.send(507, "application/json", "{\"error\":\"queue_full\"}");
    return;
  }
  server.send(200, "application/json", "{\"ok\":true}");
}

void lanAutomationEnsureStarted() {
  if (core::release_policy::productionOnly) return;
  if (s_automation_http) return;
  s_automation_http = new WebServer(AUTOMATION_HTTP_PORT);
  // ESP32 WebServer only exposes request headers that are explicitly collected;
  // without this, hasHeader("X-Api-Key") is always false and every inbound push
  // (OTA) 401s. Must be set before begin().
  static const char* kCollectHeaders[] = { "X-Api-Key" };
  s_automation_http->collectHeaders(kCollectHeaders, 1);
  s_automation_http->on(AUTOMATION_HTTP_URI, HTTP_POST, handleAutomationHttpPost);
  s_automation_http->begin();
}

void lanAutomationStop() {
  if (!s_automation_http) return;
  s_automation_http->stop();
  delete s_automation_http;
  s_automation_http = nullptr;
}

void pollAutomationsFromBackend() {
  // Keep URL and authentication from the same publication throughout I/O.
  const auto config = core::secrets::backendConfig();
  String     url = String(config.url) + String(AUTOMATION_POLL_PATH);
  const bool https = url.startsWith("https://");

  // Retain the bounded handshake while enabling shared certificate verification.
  // A deliberately configured HTTP endpoint keeps its separate plain client.
  WiFiClientSecure secClient;
  WiFiClient       plainClient;
  if (https) {
    services::tls::useRootBundle(secClient);
    secClient.setHandshakeTimeout(8); // seconds
  }
  WiFiClient& wc = https ? static_cast<WiFiClient&>(secClient)
                         : static_cast<WiFiClient&>(plainClient);

  HTTPClient http;
  services::bounded_http::configure(http);
  http.setConnectTimeout(5000);
  http.setTimeout(5000);
  if (!http.begin(wc, url)) return;
  http.addHeader("X-Api-Key", config.apiKey);
  http.setFollowRedirects(core::release_policy::productionOnly
      ? HTTPC_DISABLE_FOLLOW_REDIRECTS : HTTPC_STRICT_FOLLOW_REDIRECTS);
  const int code = http.GET();
  if (code != 200) {
    http.end();
    return;
  }
  String payload;
  const bool bodyOk = services::bounded_http::read(http, 16u * 1024u, payload);
  http.end();

  if (bodyOk) queueRecipeSnapshotJson(payload.c_str());

}

}  // namespace

void beginRecipeTransport() {
  if(!s_inbox) s_inbox=xQueueCreate(1,sizeof(services::recipe_alerts::Snapshot));
}

bool queueRecipeSnapshotJson(const char* json) {
  JsonDocument doc;
  if(deserializeJson(doc,json)) return false;
  services::recipe_alerts::Snapshot snapshot;
  if(!services::recipe_alerts::parseSnapshot(doc.as<JsonVariantConst>(),snapshot) || !s_inbox) return false;
  return xQueueOverwrite(s_inbox,&snapshot)==pdTRUE;
}

void pollTransportNow() {
  // Runs on net_poll (Core 0). Does the blocking HTTPS GET + JSON parse and
  // hands parsed entries to ui_task via the inbox. Never touches I2C or display.
  pollAutomationsFromBackend();
}

void drainPolledInbox() {
  // Runs on ui_task (Core 1). Ingest everything the poller fetched since last
  // tick, keeping ingestScheduled() single-threaded on this core.
  if (!s_inbox) return;
  services::recipe_alerts::Snapshot snapshot;
  if (xQueueReceive(s_inbox,&snapshot,0)==pdTRUE) applyRecipeSnapshot(snapshot);
}

void transport_poll_service(uint32_t ms) {
  static uint32_t s_last_handle          = 0;
  static bool     s_wifi_was_connected = false;

  const bool wifi_ok = hal::wifi::connected();
  if (wifi_ok != s_wifi_was_connected) {
    s_wifi_was_connected = wifi_ok;
    if (wifi_ok)
      lanAutomationEnsureStarted();
    else
      lanAutomationStop();
  }

  // Inbound LAN push (handleClient) and ingest stay on ui_task — both call
  // ingestScheduled() and must remain single-threaded with the rest of the
  // automation queue. The OUTBOUND periodic GET moved to net_poll (Core 0) to
  // stop blocking the display pipeline; drainPolledInbox() applies its results.
  if (s_automation_http && wifi_ok) {
    if ((uint32_t)(ms - s_last_handle) >= 50u) {
      s_last_handle = ms;
      s_automation_http->handleClient();
    }
  }

  drainPolledInbox();
}

void prepareForBatterySleep() { lanAutomationStop(); }

}  // namespace automation
}  // namespace services
