#include "tooling/tls_diagnostics.h"
#ifdef ORIGAMI_DEV_SERIAL_INPUT
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <stdio.h>
#include <string.h>
#include <lwip/sockets.h>
#include <unistd.h>
#include "services/tls_trust.h"

namespace tooling {
bool handleTlsCommand(const char* line) {
    if (strcmp(line, "tls stall-test") == 0) {
        // A loopback listener completes TCP but never answers ClientHello.
        // No credentials, application data, backend configuration or SD writes.
        const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        socklen_t size = sizeof(address);
        if (listener < 0 || bind(listener, (sockaddr*)&address, size) != 0 ||
            listen(listener, 1) != 0 || getsockname(listener, (sockaddr*)&address, &size) != 0) {
            if (listener >= 0) close(listener);
            Serial.println("[tls-stall] setup_failed=1");
            return true;
        }
        WiFiClientSecure client;
        services::tls::useRootBundle(client);
        const uint32_t started = millis();
        const bool connected = client.connect("127.0.0.1", ntohs(address.sin_port), 2500);
        const uint32_t elapsed = millis() - started;
        client.stop();
        close(listener);
        Serial.printf("[tls-stall] connected=%u ms=%lu passed=%u\n", connected,
                      (unsigned long)elapsed, !connected && elapsed >= 8000 && elapsed < 11000);
        return true;
    }
    if (strncmp(line, "tls probe ", 10) != 0) return false;
    char host[96], extra;
    unsigned port;
    if (sscanf(line+10, "%95s %u %c", host, &port, &extra) != 2 || !port || port > 65535) {
        Serial.println("[tls-probe] usage: tls probe HOST PORT");
        return true;
    }
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    uint32_t elapsed;
    size_t connected;
    int ok, error;
    {
        WiFiClientSecure client;
        services::tls::useRootBundle(client);
        const uint32_t start = millis();
        ok = client.connect(host, uint16_t(port), 4000);
        elapsed = millis()-start;
        connected = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        char detail[96];
        error = ok ? 0 : client.lastError(detail, sizeof(detail));
        client.stop();
    }
    Serial.printf("[tls-probe] connected=%d error=%d ms=%lu psram_before=%u psram_connected=%u psram_after=%u trust=%s\n",
                  ok, error, (unsigned long)elapsed, (unsigned)before,
                  (unsigned)connected, (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  services::tls::kTestTrustEnabled ? "test" : "public");
    return true;
}
}
#endif
