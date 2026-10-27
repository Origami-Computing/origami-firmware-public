/* Fault injection against the complete production TLS translation unit. */
#include <assert.h>
#include <string.h>
#include "mbedtls/sha256.h"
#include "mbedtls/sha512.h"
#include "mbedtls/md5.h"
#include "mbedtls/sha1.h"
static int hash_error, prf_error, prf_calls;
static int hash256(mbedtls_sha256_context *c, unsigned char *b) {
    return hash_error ? hash_error : mbedtls_sha256_finish_ret(c, b);
}
static int hash512(mbedtls_sha512_context *c, unsigned char *b) {
    return hash_error ? hash_error : mbedtls_sha512_finish_ret(c, b);
}
static int hashmd5(mbedtls_md5_context *c, unsigned char *b) {
    return hash_error ? hash_error : mbedtls_md5_finish_ret(c, b);
}
static int hashsha1(mbedtls_sha1_context *c, unsigned char *b) {
    return hash_error ? hash_error : mbedtls_sha1_finish_ret(c, b);
}
#define mbedtls_sha256_finish_ret hash256
#define mbedtls_sha512_finish_ret hash512
#define mbedtls_md5_finish_ret hashmd5
#define mbedtls_sha1_finish_ret hashsha1
#include "../third_party/mbedtls-finished/library/ssl_tls.c"

static int test_prf(const unsigned char *secret, size_t slen, const char *label,
                    const unsigned char *random, size_t rlen,
                    unsigned char *dst, size_t dlen) {
    (void)secret; (void)slen; (void)label; (void)random; (void)rlen;
    ++prf_calls;
    if (prf_error) return prf_error;
    memset(dst, 0x5a, dlen);
    return 0;
}
static int fail_finished(mbedtls_ssl_context *s, unsigned char *b, int role) {
    (void)s; (void)b; (void)role;
    return -0x1234;
}
int main(void) {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ssl_init(&ssl);
    mbedtls_ssl_config_init(&conf);
    assert(mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT,
           MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) == 0);
    assert(mbedtls_ssl_setup(&ssl, &conf) == 0);
    ssl.handshake->tls_prf = test_prf;
    int (*functions[])(mbedtls_ssl_context *, unsigned char *, int) = {
        ssl_calc_finished_tls_sha256, ssl_calc_finished_tls_sha384,
        ssl_calc_finished_tls
    };
    unsigned char output[12];
    for (unsigned i = 0; i < sizeof(functions)/sizeof(functions[0]); ++i) {
        memset(output, 0xa5, sizeof(output));
        hash_error = -0x1234; prf_calls = 0;
        assert(functions[i](&ssl, output, MBEDTLS_SSL_IS_CLIENT) == hash_error);
        assert(prf_calls == 0);
        for (unsigned j=0; j<sizeof(output); ++j) assert(output[j] == 0xa5);
        hash_error = 0; prf_error = -0x5678;
        assert(functions[i](&ssl, output, MBEDTLS_SSL_IS_CLIENT) == prf_error);
        assert(prf_calls == 1);
        prf_error = 0;
        assert(functions[i](&ssl, output, MBEDTLS_SSL_IS_SERVER) == 0);
        for (unsigned j=0; j<sizeof(output); ++j) assert(output[j] == 0x5a);
    }
    ssl.handshake->calc_finished = fail_finished;
    const int state = ssl.state;
    assert(mbedtls_ssl_parse_finished(&ssl) == -0x1234);
    assert(ssl.state == state);
    assert(mbedtls_ssl_write_finished(&ssl) == -0x1234);
    assert(ssl.state == state);
    mbedtls_ssl_free(&ssl);
    mbedtls_ssl_config_free(&conf);
    return 0;
}
