# Reproducible TLS Finished repair

This directory contains `ssl_tls.c`, `common.h` and `ssl_internal.h` from
Espressif Mbed TLS commit `2b8e772fc1cb0732cda3bae7d1e9d6f4cfaf63d9`, matching
the pinned Arduino SDK. The upstream internal header was byte-compared with
the installed package before applying the patch. Use the Apache-2.0 option
offered by the original dual-license headers; the complete LICENSE is retained.

Applied upstream Mbed TLS commit
[`6070470`](https://github.com/Mbed-TLS/mbedtls/commit/6070470dfd5c680a59a55bc55299e1a65a6c7049)
(retained as `upstream-6070470.patch`): return and propagate Finished errors,
including PSA errors, from both read and write paths. Additionally, this SDK's
non-PSA SHA/MD5 finish and PRF errors are propagated for TLS 1.0–1.2; temporary
hash contexts and buffers are cleaned up on failure. The source uses its local
patched internal header. No structure layout or public function ABI is changed.

`src/hal/mbedtls_finished.c` compiles this whole translation unit as a direct
application object. It resolves the symbols before the SDK archive, so the
precompiled `ssl_tls.c.obj` must not appear in the linked image. No installed
package cache is modified. Recheck the link map after every SDK upgrade.

`python3 tools/test_tls_finished.py` compiles the actual translation unit and
injects hash/PRF failures for SHA-256, SHA-384 and legacy TLS. It checks both
Finished entry points abort without advancing the handshake. Host checks do
not replace hardware TLS/OTA qualification. This focused backport does not
claim to upgrade the entire SDK or resolve every dependency advisory.
