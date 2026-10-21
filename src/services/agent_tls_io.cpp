#include "services/agent_coordination.h"
#include "core/telemetry.h"
#include <esp_arduino_version.h>
#include <mbedtls/ssl.h>

// WiFiClientSecure clears the failed TLS context before the WebSocket disconnect
// callback. Capture the actual code at that boundary, without changing retries,
// return values, socket ownership, certificate checks or any other TLS caller.
static_assert(ESP_ARDUINO_VERSION == ESP_ARDUINO_VERSION_VAL(2, 0, 17),
              "Revalidate Agent TLS I/O diagnostics against the upgraded core");
namespace {
int report(const char* operation, int result) {
    if (result < 0 && result != MBEDTLS_ERR_SSL_WANT_READ &&
        result != MBEDTLS_ERR_SSL_WANT_WRITE && services::agent_coordination::isCurrentWorker()) {
        LOGW("agent-tls", "op=%s code=%d", operation, result);
    }
    return result;
}
}
extern "C" int __real_mbedtls_ssl_read(mbedtls_ssl_context*, unsigned char*, size_t);
extern "C" int __real_mbedtls_ssl_write(mbedtls_ssl_context*, const unsigned char*, size_t);
extern "C" int __wrap_mbedtls_ssl_read(mbedtls_ssl_context* ssl, unsigned char* data, size_t length) {
    return report("read", __real_mbedtls_ssl_read(ssl, data, length));
}
extern "C" int __wrap_mbedtls_ssl_write(mbedtls_ssl_context* ssl, const unsigned char* data, size_t length) {
    return report("write", __real_mbedtls_ssl_write(ssl, data, length));
}
