#include "services/tls_trust.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <atomic>
#include <cstring>
#include <esp_err.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>

#ifdef KAMI_TLS_TEST_TRUST
extern const uint8_t roots[] asm("_binary__tls_test_roots_bundle_start");
#else
extern const uint8_t roots[] asm("_binary_certs_mozilla_bundle_start");
#endif

// Verification against the pre-indexed bundle (tools/gen_crt_bundle.py): count,
// then per root (sorted by DER subject) subject and SubjectPublicKeyInfo.
//
// Arduino's own bundle support is replaced through linker wraps because it
// (1) keeps one process-wide index that every setCACertBundle() frees and
// rebuilds under other tasks' in-flight handshakes, and (2) only trusts a chain
// whose top certificate's ISSUER is a bundle root. Servers commonly send a
// trusted root cross-signed by a retired one (api.kami.so sends GTS Root R4
// signed by GlobalSign Root CA, which Mozilla removed); PEM verification
// accepted that because GTS Root R4 itself is trusted. The callback below
// accepts both: the issuer's signature, or the certificate being a bundle root.
namespace {
constexpr size_t kMaxRoots = 256;
const uint8_t* s_entries[kMaxRoots];
size_t s_count = 0;
std::atomic<bool> s_ready{false};
mbedtls_x509_crt s_dummyCa;

size_t nameLen(const uint8_t* e) { return (size_t)(e[0] << 8 | e[1]); }
size_t keyLen(const uint8_t* e) { return (size_t)(e[2] << 8 | e[3]); }
const uint8_t* nameOf(const uint8_t* e) { return e + 4; }
const uint8_t* keyOf(const uint8_t* e) { return e + 4 + nameLen(e); }

void buildIndex() {
    bool expected = false;
    static std::atomic<bool> claimed{false};
    if (!claimed.compare_exchange_strong(expected, true)) {
        while (!s_ready.load()) vTaskDelay(1);
        return;
    }
    const size_t count = (size_t)(roots[0] << 8 | roots[1]);
    const uint8_t* cursor = roots + 2;
    for (size_t i = 0; i < count && i < kMaxRoots; ++i) {
        s_entries[i] = cursor;
        cursor += 4 + nameLen(cursor) + keyLen(cursor);
    }
    s_count = count < kMaxRoots ? count : kMaxRoots;
    mbedtls_x509_crt_init(&s_dummyCa);
    s_ready.store(true);
}

// Binary search by exact DER name (bundle order is bytewise on the full name).
const uint8_t* findRoot(const mbedtls_x509_buf& name) {
    size_t lo = 0, hi = s_count;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        const uint8_t* e = s_entries[mid];
        const size_t len = nameLen(e);
        const size_t common = len < name.len ? len : name.len;
        int cmp = memcmp(nameOf(e), name.p, common);
        if (cmp == 0) cmp = len < name.len ? -1 : len > name.len ? 1 : 0;
        if (cmp == 0) return e;
        if (cmp < 0) lo = mid + 1; else hi = mid;
    }
    return nullptr;
}

bool signedByRoot(const mbedtls_x509_crt* child, const uint8_t* root) {
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    bool ok = mbedtls_pk_parse_public_key(&key, keyOf(root), keyLen(root)) == 0;
    const mbedtls_md_info_t* md = mbedtls_md_info_from_type(child->sig_md);
    unsigned char hash[MBEDTLS_MD_MAX_SIZE];
    ok = ok && md && mbedtls_md(md, child->tbs.p, child->tbs.len, hash) == 0 &&
         mbedtls_pk_verify_ext(child->sig_pk, child->sig_opts, &key, child->sig_md,
                               hash, mbedtls_md_get_size(md), child->sig.p, child->sig.len) == 0;
    mbedtls_pk_free(&key);
    return ok;
}

bool isRoot(mbedtls_x509_crt* crt, const uint8_t* root) {
    // mbedtls_pk_write_pubkey_der writes at the END of the buffer. The largest
    // bundle key is an RSA-4096 SubjectPublicKeyInfo (550 B); a longer key can
    // never match, and this runs on worker stacks with ~2 KB of headroom.
    unsigned char der[600];
    const int written = mbedtls_pk_write_pubkey_der(&crt->pk, der, sizeof(der));
    return written > 0 && (size_t)written == keyLen(root) &&
           memcmp(der + sizeof(der) - written, keyOf(root), (size_t)written) == 0;
}

int verifyAgainstBundle(void*, mbedtls_x509_crt* crt, int, uint32_t* flags) {
    // Only an exact trusted root may ignore its own weak signature digest.
    // A child still needs a strong signature, even when its issuer is trusted.
    if ((*flags & ~MBEDTLS_X509_BADCERT_BAD_MD) != MBEDTLS_X509_BADCERT_NOT_TRUSTED) return 0;
    const uint8_t* issuer = findRoot(crt->issuer_raw);
    if (*flags == MBEDTLS_X509_BADCERT_NOT_TRUSTED && issuer && signedByRoot(crt, issuer)) { *flags = 0; return 0; }
    const uint8_t* self = findRoot(crt->subject_raw);
    if (self && isRoot(crt, self)) { *flags = 0; return 0; }
    return 0; // Leave NOT_TRUSTED; the handshake fails with a verification error.
}
}  // namespace

extern "C" void __wrap_arduino_esp_crt_bundle_set(const uint8_t* bundle) {
    if (bundle != roots) Serial.println("[tls] ignored a foreign certificate bundle");
    buildIndex();
}

extern "C" void __wrap_arduino_esp_crt_bundle_detach(void*) {
    // The index is immutable for the life of the firmware.
}

extern "C" esp_err_t __wrap_arduino_esp_crt_bundle_attach(void* conf) {
    buildIndex();
    if (conf) {
        auto* ssl = static_cast<mbedtls_ssl_config*>(conf);
        // A non-null CA chain satisfies mbedTLS's pre-check; trust is decided
        // entirely by the callback.
        mbedtls_ssl_conf_ca_chain(ssl, &s_dummyCa, nullptr);
        mbedtls_ssl_conf_verify(ssl, verifyAgainstBundle, nullptr);
    }
    return ESP_OK;
}

namespace services { namespace tls {
const uint8_t* rootBundle() { return roots; }
void useRootBundle(WiFiClientSecure& client) {
    client.setCACertBundle(roots);
    // TCP connect timeouts do not bound Arduino's separate 120-second TLS
    // handshake default. Every shared-trust caller needs a finite handshake.
    client.setHandshakeTimeout(8); // seconds; below the 15-second worker watchdog
}
void init() {
    buildIndex();
    Serial.printf("[tls] root bundle ready: %u roots\n", (unsigned)s_count);
}
#ifdef KAMI_TLS_TEST_TRUST
const bool kTestTrustEnabled = true;
#else
const bool kTestTrustEnabled = false;
#endif
}}
