#include "core/wifi_qr_parser.h"

#include <string.h>

namespace core { namespace wifi_qr {
namespace {
bool unescape(const char* in, size_t length, char* out, size_t capacity) {
  size_t n = 0;
  for (size_t i = 0; i < length; ++i) {
    if (n + 1 >= capacity) return false;
    if (in[i] == '\\') {
      if (++i >= length) return false;
    }
    out[n++] = in[i];
  }
  out[n] = '\0';
  return true;
}
}

bool validCredentials(const Credentials& credentials) {
  const size_t ssidLen = strnlen(credentials.ssid, sizeof(credentials.ssid));
  const size_t passLen = strnlen(credentials.pass, sizeof(credentials.pass));
  if (ssidLen == 0 || ssidLen > 32 || passLen > 64) return false;
  if (passLen == 0) return true;
  if (passLen < 8) return false;
  if (passLen == 64) {
    for (size_t i = 0; i < passLen; ++i) {
      const char c = credentials.pass[i];
      if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
  }
  return true;
}

bool parseStandardQr(const char* text, Credentials* out) {
  if (!text || !out || strncmp(text, "WIFI:", 5) != 0) return false;
  *out = {};
  const char* p = text + 5;
  bool haveS = false, haveP = false, haveT = false;
  char auth[16] = {};
  while (*p && *p != ';') {
    const char tag = *p++;
    if (*p != ':') return false;
    const char* start = ++p;
    while (*p && *p != ';') {
      if (*p == '\\' && p[1]) ++p;
      ++p;
    }
    const size_t length = (size_t)(p - start);
    // Process the field BEFORE consuming the final double-semicolon.
    if (tag == 'S') {
      if (haveS || !unescape(start, length, out->ssid, sizeof(out->ssid))) return false;
      haveS = true;
    } else if (tag == 'P') {
      if (haveP || !unescape(start, length, out->pass, sizeof(out->pass))) return false;
      haveP = true;
    } else if (tag == 'T') {
      if (haveT || !unescape(start, length, auth, sizeof(auth))) return false;
      haveT = true;
      if (strcmp(auth, "WPA") && strcmp(auth, "WPA2") && strcmp(auth, "WPA3") && strcmp(auth, "nopass")) return false;
    } else if (tag == 'K') {
      if (!unescape(start, length, out->apiKey, sizeof(out->apiKey))) return false;
      out->hasApiKey = out->apiKey[0] != '\0';
    } else if (tag != 'H') {
      // Enterprise identity/EAP fields must not be silently treated as PSK.
      return false;
    }
    if (*p == ';') ++p;
  }
  while (*p == ';') ++p;
  if (*p || !haveS || !validCredentials(*out)) return false;
  if (haveT && strcmp(auth, "nopass") == 0) return !out->pass[0];
  return !haveT || out->pass[0];
}
} }
