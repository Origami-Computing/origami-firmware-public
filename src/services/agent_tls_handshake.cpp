#include "services/agent_coordination.h"
#include "services/net_poll.h"
#include <esp_arduino_version.h>
#include <mbedtls/ssl.h>

// Arduino 2.0.17 retries WANT_READ/WANT_WRITE on its nonblocking socket.
// Return a terminal error between retries so WiFiClientSecure::connect performs
// its normal stop() cleanup. Only the owning Agent or network poll worker can
// cancel here; stop/pause acknowledgment still follows transport cleanup.
// Never terminate a task or close another task's socket from this boundary.
static_assert(ESP_ARDUINO_VERSION == ESP_ARDUINO_VERSION_VAL(2, 0, 17),
              "Revalidate Agent TLS cancellation against the upgraded core");
extern "C" int __real_mbedtls_ssl_handshake(mbedtls_ssl_context* ssl);
extern "C" int __wrap_mbedtls_ssl_handshake(mbedtls_ssl_context* ssl) {
    if (services::agent_coordination::cancelHandshakeForCurrentTask() ||
        services::net_poll::cancelHandshakeForCurrentTask())
        return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return __real_mbedtls_ssl_handshake(ssl);
}
