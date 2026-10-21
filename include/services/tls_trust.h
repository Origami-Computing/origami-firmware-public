#pragma once
#include <stdint.h>

class WiFiClientSecure;

namespace services { namespace tls {
/** Public CA trust for backend HTTPS and Agent WSS: the pinned Mozilla store
 * (certs/mozilla.pem) compiled into an ESP-IDF certificate bundle
 * (certs/mozilla.bundle, tools/gen_crt_bundle.py). A handshake looks up the one
 * root that signed the chain's top certificate by binary search and checks that
 * signature; the full PEM store is never parsed on a connection. Hostname,
 * signature and (wrapped) date checks remain enabled by the TLS library.
 * Plain HTTP/WS remains an explicit development endpoint choice, never a TLS
 * failure fallback. OTA/assets retain their existing narrower trust anchors. */
const uint8_t* rootBundle();
/** Verify `client` against rootBundle() and set an 8-second handshake deadline.
 * TCP/DNS/request budgets are separate. Call before connect, from the owning
 * task; never reconfigure a client concurrently with its I/O. */
void useRootBundle(WiFiClientSecure& client);
/** Build the bundle index once, before network tasks start. Idempotent. */
void init();
/** True only in the explicitly named local certificate-test build. */
extern const bool kTestTrustEnabled;
}}
