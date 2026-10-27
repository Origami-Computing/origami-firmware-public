#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <esp_arduino_version.h>
#include <freertos/task.h>
#include <lwip/sockets.h>
#include <mbedtls/net_sockets.h>
#include <cstring>
#include <cerrno>

// Opt in for a diagnostic build; ordinary releases must not log every TLS
// record or add serial I/O to the upload hot path.
#ifndef ORIGAMI_TLS_TRACE
#define ORIGAMI_TLS_TRACE 0
#endif

// Observe the pinned SDK boundary without changing certificates, socket ownership
// or error codes. DNS has already completed when this function is entered.
static_assert(ESP_ARDUINO_VERSION == ESP_ARDUINO_VERSION_VAL(2, 0, 17),
              "Revalidate TLS connection tracing on SDK upgrade");
extern "C" int __real__Z16start_ssl_clientP17sslclient_contextRK9IPAddressjPKciS5_bS5_S5_S5_S5_bPS5_(
    sslclient_context*, const IPAddress&, uint32_t, const char*, int, const char*,
    bool, const char*, const char*, const char*, const char*, bool, const char**);
extern "C" int __wrap__Z16start_ssl_clientP17sslclient_contextRK9IPAddressjPKciS5_bS5_S5_S5_S5_bPS5_(
    sslclient_context* c, const IPAddress& ip, uint32_t port, const char* host,
    int timeout, const char* ca, bool bundle, const char* cert, const char* key,
    const char* identity, const char* psk, bool insecure, const char** alpn) {
#if ORIGAMI_TLS_TRACE
  const uint32_t started = millis();
  Serial.printf("[tls-connect] begin task=%s host=%s ms=%lu int=%u largest=%u\n",
    pcTaskGetName(nullptr), host ? host : "ip", (unsigned long)started,
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
#endif
  const int result = __real__Z16start_ssl_clientP17sslclient_contextRK9IPAddressjPKciS5_bS5_S5_S5_S5_bPS5_(
      c, ip, port, host, timeout, ca, bundle, cert, key, identity, psk, insecure, alpn);
#if ORIGAMI_TLS_TRACE
  Serial.printf("[tls-connect] end task=%s result=%d state=%d elapsed=%lu int=%u largest=%u\n",
    pcTaskGetName(nullptr), result, c->ssl_ctx.state, (unsigned long)(millis()-started),
    (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT),
    (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT));
  sockaddr_in local={}; socklen_t localLen=sizeof(local);
  getsockname(c->socket,reinterpret_cast<sockaddr*>(&local),&localLen);
  Serial.printf("[tls-socket] task=%s fd=%d local_port=%u remote=%s verify=%lu out_left=%u in_left=%u\n",
    pcTaskGetName(nullptr),c->socket,unsigned(ntohs(local.sin_port)),ip.toString().c_str(),
    (unsigned long)mbedtls_ssl_get_verify_result(&c->ssl_ctx),unsigned(c->ssl_ctx.out_left),unsigned(c->ssl_ctx.in_left));
#endif
  return result;
}

extern "C" int __real_mbedtls_net_send(void*, const unsigned char*, size_t);
extern "C" int __real_mbedtls_net_recv(void*, unsigned char*, size_t);
namespace {
void traceWire(const char* op, void* ctx, size_t length, int result) {
#if ORIGAMI_TLS_TRACE
  if (strcmp(pcTaskGetName(nullptr),"uploadq") || result==MBEDTLS_ERR_SSL_WANT_READ || result==MBEDTLS_ERR_SSL_WANT_WRITE) return;
  Serial.printf("[tls-wire] ms=%lu op=%s fd=%d requested=%u result=%d errno=%d\n",
    (unsigned long)millis(),op,*static_cast<int*>(ctx),unsigned(length),result,result < 0 ? errno : 0);
#else
  (void)op; (void)ctx; (void)length; (void)result;
#endif
}
}
extern "C" int __wrap_mbedtls_net_send(void* ctx,const unsigned char* buf,size_t len) {
 int result=__real_mbedtls_net_send(ctx,buf,len);traceWire("send",ctx,len,result);return result;
}
extern "C" int __wrap_mbedtls_net_recv(void* ctx,unsigned char* buf,size_t len) {
 int result=__real_mbedtls_net_recv(ctx,buf,len);traceWire("recv",ctx,len,result);return result;
}
