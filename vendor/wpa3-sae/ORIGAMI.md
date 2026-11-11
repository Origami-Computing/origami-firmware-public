# Pinned WPA3 SAE cleanup replacement

This directory contains the SAE translation unit and its private headers from
[ESP-IDF v4.4.7](https://github.com/espressif/esp-idf/tree/v4.4.7/components/wpa_supplicant).
The original file hashes and source URL are in `upstream-sha256.json`; the upstream
BSD license is retained in `README`. Platform headers, crypto implementation and
all other supplicant objects still come from the pinned Arduino 2.0.17 SDK.

`src/hal/wpa3_sae_compat.c` compiles this translation unit with the upstream
component definitions. Its strong SAE definitions satisfy references before the
SDK archive, so the archive's original `sae.c.obj` is not extracted. Check the
firmware map after framework/linker changes. The wrapper rejects a different
Arduino/IDF version; review and remove this replacement when upgrading to a
vendor release that fixes the leak. No package cache is modified and builds do
not fetch any source from the network.

## Exact correction

In `sswu`, the pinned source allocates coefficient `a` with
`crypto_bignum_init_set(buf, 32)` but treats it like the borrowed `b` and `prime`
and never frees it. Each WPA3 H2E derivation invokes `sswu` twice. COM38 allocation
tracing identified two retained 32-byte MPI buffers each reconnect: 64 requested
bytes plus 32 allocator-overhead bytes account for the observed 96-byte PSRAM
loss. The owning bignum containers are also released by the correction.

The only changes to upstream SAE are:

- Declare `a` as an owned mutable pointer, separate from borrowed constants.
- Include `a` in the allocation-failure guard.
- Free `a` at the common success/failure cleanup label.

No authentication arithmetic, protocol selection, certificate policy, task stack,
or radio timing is changed. All private headers remain byte-for-byte upstream.
`tools/test_wpa3_sae_cleanup.py` verifies those hashes and the exact source delta,
reproduces the original leak, and checks the actual SSWU control flow under
sanitizers with arithmetic mocked. Real WPA3 joins and allocation traces provide
the complementary device verification.
