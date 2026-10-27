#pragma once
#include <Arduino.h>
#include <HTTPClient.h>

namespace services { namespace bounded_http {
// Use HTTP/1.0 to request a finite body. Unknown/chunked bodies fail closed;
// never call HTTPClient::getString before enforcing the memory budget.
inline void configure(HTTPClient& http) {
    http.useHTTP10(true);
    http.setReuse(false);
    http.setTimeout(4000);
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    static const char* headers[] = {"Transfer-Encoding"};
    http.collectHeaders(headers, 1);
}
inline bool read(HTTPClient& http, size_t limit, String& out) {
    out = "";
    const int length = http.getSize();
    if (length < 0 || static_cast<size_t>(length) > limit ||
        http.header("Transfer-Encoding").length() != 0) return false;
    if (!out.reserve(static_cast<unsigned int>(length))) return false;
    auto* stream = http.getStreamPtr();
    if (!stream) return false;
    const uint32_t started = millis();
    while (out.length() < static_cast<size_t>(length)) {
        if (static_cast<uint32_t>(millis() - started) >= 10000u) { out = ""; return false; }
        const int available = stream->available();
        if (available <= 0) {
            if (!http.connected()) { out = ""; return false; }
            delay(1);
            continue;
        }
        char buffer[256];
        size_t count = static_cast<size_t>(length) - out.length();
        if (count > sizeof(buffer)) count = sizeof(buffer);
        if (count > static_cast<size_t>(available)) count = static_cast<size_t>(available);
        const int got = stream->read(reinterpret_cast<uint8_t*>(buffer), count);
        if (got <= 0 || !out.concat(buffer, static_cast<unsigned int>(got))) { out = ""; return false; }
    }
    return true;
}
}}
