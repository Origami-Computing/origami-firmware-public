#include "services/automation.h"
#include <Arduino.h>
#include <ArduinoJson.h>
#include <strings.h>

namespace services { namespace automation {
void ingestBleNotification(const char* json) {
  JsonDocument doc;
  if (deserializeJson(doc, json)) {
    Serial.println("[ble] bad notification JSON");
    return;
  }

  const char* typeStr = doc["type"] | "yellow";
  const char* message = doc["message"] | "BLE Notification";
  time_t fireTime     = (time_t)(doc["time"] | (long long)0);

  services::automation::Type type = services::automation::Type::Yellow;
  if (strcasecmp(typeStr, "red") == 0)   type = services::automation::Type::Red;
  if (strcasecmp(typeStr, "green") == 0) type = services::automation::Type::Green;

  if (fireTime < 1000000000) fireTime = time(nullptr) + 3;

  if (services::automation::schedule(type, fireTime, message)) {
    Serial.printf("[ble] notification scheduled: \"%s\"\n", message);
  } else {
    Serial.println("[ble] notification schedule failed (queue full?)");
  }
}
} }
