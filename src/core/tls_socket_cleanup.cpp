#include <WiFiClientSecure.h>
#include <esp_arduino_version.h>

#include "core/tls_socket_cleanup.h"
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <lwip/sockets.h>
#include <lwip/tcpip.h>
#include <lwip/priv/tcpip_priv.h>
#include <lwip/priv/tcp_priv.h>

namespace {
struct Endpoint {
  sslclient_context* owner = nullptr;
  uint32_t local = 0, remote = 0;
  uint16_t localPort = 0, remotePort = 0;
};
Endpoint endpoints[CONFIG_LWIP_MAX_SOCKETS];
portMUX_TYPE endpointMux = portMUX_INITIALIZER_UNLOCKED;
struct ClosedConnection { tcpip_api_call_data call; Endpoint endpoint; };
err_t retireClosedConnection(tcpip_api_call_data* data) {
  const auto& e = reinterpret_cast<ClosedConnection*>(data)->endpoint;
  // tcpip_thread only, after SDK close. Preserve live connections and TIME_WAIT.
  for (auto* p = tcp_active_pcbs; p; p = p->next) {
    if (p->state != FIN_WAIT_1 && p->state != FIN_WAIT_2 &&
        p->state != CLOSING && p->state != LAST_ACK) continue;
    if (!IP_IS_V4(&p->local_ip) || !IP_IS_V4(&p->remote_ip)) continue;
    if (ip4_addr_get_u32(ip_2_ip4(&p->local_ip)) == e.local &&
        ip4_addr_get_u32(ip_2_ip4(&p->remote_ip)) == e.remote &&
        p->local_port == e.localPort && p->remote_port == e.remotePort) {
      tcp_abort(p);
      break;
    }
  }
  return ERR_OK;
}
Endpoint takeEndpoint(sslclient_context* owner) {
  Endpoint result;
  portENTER_CRITICAL(&endpointMux);
  for (auto& e : endpoints) if (e.owner == owner) { result = e; e = {}; break; }
  portEXIT_CRITICAL(&endpointMux);
  return result;
}
}
namespace core { namespace tls {
void rememberSocket(sslclient_context* client) {
  if (!client || client->socket < 0) return;
  sockaddr_in local{}, remote{};
  socklen_t localSize = sizeof(local), remoteSize = sizeof(remote);
  if (getsockname(client->socket, reinterpret_cast<sockaddr*>(&local), &localSize) ||
      getpeername(client->socket, reinterpret_cast<sockaddr*>(&remote), &remoteSize) ||
      local.sin_family != AF_INET || remote.sin_family != AF_INET) return;
  Endpoint saved;
  saved.owner = client;
  saved.local = local.sin_addr.s_addr; saved.remote = remote.sin_addr.s_addr;
  saved.localPort = ntohs(local.sin_port); saved.remotePort = ntohs(remote.sin_port);
  portENTER_CRITICAL(&endpointMux);
  Endpoint* slot = nullptr;
  for (auto& e : endpoints) {
    if (e.owner == client) { slot = &e; break; }
    if (!e.owner && !slot) slot = &e;
  }
  if (slot) *slot = saved;
  portEXIT_CRITICAL(&endpointMux);
}
} }

// Arduino-ESP32 2.0.17 stop_ssl_socket() clears its entire context, including
// socket, to zero. WiFiClientSecure::stop() followed by its destructor then
// calls close(0), closing an unrelated VFS file (often the live WAV). Keep this
// compatibility fix in the project, not in a machine-global PlatformIO package.
// The linker wraps the exact C++ symbol called by WiFiClientSecure::stop().
// Revisit the wrapper and linker flag together when upgrading Arduino.
static_assert(ESP_ARDUINO_VERSION == ESP_ARDUINO_VERSION_VAL(2, 0, 17),
              "Revalidate TLS socket cleanup against the upgraded Arduino core");

extern "C" void __real__Z15stop_ssl_socketP17sslclient_contextPKcS2_S2_(
    sslclient_context*, const char*, const char*, const char*);

extern "C" void __wrap__Z15stop_ssl_socketP17sslclient_contextPKcS2_S2_(
    sslclient_context* client, const char* ca, const char* cert, const char* key) {
  const Endpoint endpoint = takeEndpoint(client);
  __real__Z15stop_ssl_socketP17sslclient_contextPKcS2_S2_(client, ca, cert, key);
  client->socket = -1;
  if (endpoint.owner) {
    ClosedConnection closed{};
    closed.endpoint = endpoint;
    // Stack payload remains alive until completion. Failed callback admission
    // leaves ordinary lwIP close/retransmission cleanup in place.
    (void)tcpip_api_call(retireClosedConnection, &closed.call);
  }
}
