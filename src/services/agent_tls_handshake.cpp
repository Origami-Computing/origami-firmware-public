#include "services/agent_coordination.h"
#include <esp_arduino_version.h>
#include <mbedtls/ssl.h>

// Arduino 2.0.17 retries WANT_READ/WANT_WRITE on its nonblocking socket.
// Return a terminal error between retries so WiFiClientSecure::connect performs
// its normal stop() cleanup. Only the persistent Agent worker can cancel here;
// a pause is acknowledged later, after the transport's disconnect has finished.
// Never terminate a task or close another task's socket from this boundary.
static_assert(ESP_ARDUINO_VERSION == ESP_ARDUINO_VERSION_VAL(2, 0, 17),
              "Revalidate Agent TLS cancellation against the upgraded core");
extern "C" int __real_mbedtls_ssl_handshake(mbedtls_ssl_context* ssl);
extern "C" int __wrap_mbedtls_ssl_handshake(mbedtls_ssl_context* ssl) {
    if (services::agent_coordination::cancelHandshakeForCurrentTask())
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return __real_mbedtls_ssl_handshake(ssl);
}
