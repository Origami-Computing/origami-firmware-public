#include "core/crash_report.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <string.h>
#include <time.h>

#include "core/device_id.h"
#include "core/telemetry.h"
#include "core/telemetry_queue.h"
#include "core/watchdog.h"

namespace core {
namespace crash_report {

namespace {

const char* resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWERON";
    case ESP_RST_EXT:       return "EXT";
    case ESP_RST_SW:        return "SW";
    case ESP_RST_PANIC:     return "PANIC";
    case ESP_RST_INT_WDT:   return "INT_WDT";
    case ESP_RST_TASK_WDT:  return "TASK_WDT";
    case ESP_RST_WDT:       return "OTHER_WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

// Reasons worth reporting as a crash (vs. clean power-on / software restart).
bool isFault(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT ||
         r == ESP_RST_WDT   || r == ESP_RST_BROWNOUT;
}

}  // namespace

void reportBootReason() {
  const esp_reset_reason_t reason = esp_reset_reason();
  const char* reasonStr = resetReasonStr(reason);
  Serial.printf("[crash] reset_reason=%s\n", reasonStr);

  if (!isFault(reason)) return;

  const bool isWdt = (reason == ESP_RST_TASK_WDT || reason == ESP_RST_INT_WDT ||
                      reason == ESP_RST_WDT);
  // Prefer the watchdog's stalest-task guess for a WDT reset; fall back to the
  // breadcrumb hung-task field if a TWDT callback recorded one.
  const char* hung = isWdt ? watchdog::hungTaskGuess() : "";
  if ((!hung || !hung[0]) && telemetry::breadcrumbValid()) {
    hung = telemetry::breadcrumbHungTask();
  }

  JsonDocument doc;
  doc["device_id"]    = core::device::id();
  doc["fw"]           = core::device::firmwareVersion();
  doc["reset_reason"] = reasonStr;
  doc["uptime_ms"]    = (uint32_t)millis();  // ~0 at boot: marks a fresh crash report
  if (hung && hung[0]) doc["hung_task"] = hung;
  if (telemetry::breadcrumbValid() && telemetry::breadcrumbLastTag()[0]) {
    doc["last_tag"] = telemetry::breadcrumbLastTag();
  }

  // Attach the recent log ring (best-effort; may be empty after a cold ring).
  char logs[256];
  if (telemetry::copyRecentLogs(logs, sizeof(logs)) > 0) {
    JsonArray arr = doc["recent_logs"].to<JsonArray>();
    char* line = strtok(logs, "\n");
    while (line) { arr.add(line); line = strtok(nullptr, "\n"); }
  }

  char json[telemetry_queue::kItemJsonCap];
  serializeJson(doc, json, sizeof(json));
  telemetry_queue::enqueue(telemetry_queue::KIND_CRASH, json);
  telemetry::clearBreadcrumbCrash();
  Serial.printf("[crash] reported %s hung='%s'\n", reasonStr, hung ? hung : "");
}

}  // namespace crash_report
}  // namespace core
