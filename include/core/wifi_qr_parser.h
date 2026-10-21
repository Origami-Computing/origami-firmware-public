#pragma once
#include <stddef.h>

namespace core { namespace wifi_qr {
struct Credentials {
  char ssid[33];
  char pass[65];
  char apiKey[192];
  bool hasApiKey;
};

/**
 * Parse a scanned QR payload into WiFi + optional API key fields.
 *
 * Supported formats:
 *   - Standard WiFi QR: WIFI:T:WPA;S:<ssid>;P:<pass>;;
 *   - Origami extension: ...;K:<api_key>;;  (before the trailing ;;)
 *   - JSON: {"ssid":"...","pass":"...","key":"..."}  (also password / api_key aliases)
 */
bool parsePayload(const char* text, Credentials* out);

bool parseStandardQr(const char* text, Credentials* out);
bool validCredentials(const Credentials& credentials);
} }
