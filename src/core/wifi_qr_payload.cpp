#include "core/wifi_qr_parser.h"

#include <ArduinoJson.h>
#include <string.h>


namespace core {
namespace wifi_qr {
namespace {

static void copyField(char* dst, size_t cap, const char* src) {
  if (!dst || cap == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = '\0';
}

static bool parseJsonQr(const char* text, Credentials* out) {
  if (!text || text[0] != '{') return false;

  JsonDocument doc;
  if (deserializeJson(doc, text)) return false;

  const char* ssid = doc["ssid"] | doc["S"] | doc["s"] | "";
  const char* pass = doc["pass"] | doc["password"] | doc["P"] | doc["p"] | "";
  const char* key  = doc["key"] | doc["api_key"] | doc["apiKey"] | doc["K"] | doc["a"] | "";

  // Fallback: nested wifi object {"wifi":{"ssid":"...","password":"..."}}
  if (!ssid || !ssid[0]) {
    ssid = doc["wifi"]["ssid"] | doc["wifi"]["S"] | doc["wifi"]["s"] | "";
    if (!pass || !pass[0])
      pass = doc["wifi"]["password"] | doc["wifi"]["pass"] | doc["wifi"]["P"] | doc["wifi"]["p"] | "";
  }
  if (!ssid || !ssid[0]) return false;
  if (strlen(ssid) >= sizeof(out->ssid) || strlen(pass) >= sizeof(out->pass) ||
      strlen(key) >= sizeof(out->apiKey)) return false;

  copyField(out->ssid, sizeof(out->ssid), ssid);
  copyField(out->pass, sizeof(out->pass), pass);
  copyField(out->apiKey, sizeof(out->apiKey), key);
  out->hasApiKey = out->apiKey[0] != '\0';
  return validCredentials(*out);
}

}  // namespace

bool parsePayload(const char* text, Credentials* out) {
  if (!text || !out) return false;
  memset(out, 0, sizeof(*out));

  if (parseStandardQr(text, out)) return true;
  if (parseJsonQr(text, out)) return true;
  return false;
}

}  // namespace wifi_qr
}  // namespace core
