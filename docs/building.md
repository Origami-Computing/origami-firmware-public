# Building and verification

Use PlatformIO Core 6.2.0 and the `esp32-s3-menu-n16r8-production` environment. `platformio.ini` retains the N16R8 dependency chain and custom production-policy environment. Development-board, test-image, integration and cloud publishing flows were removed from this draft. A production compile-policy reproduction environment is documented in [distribution](distribution.md).

The pinned build uses Espressif32 6.12.0, Arduino package `3.20017.241212+sha.dcc1105b`, esptool package `2.40900.250804`, and explicit library versions. The locally maintained libraries under `lib/` are required. A new build is not claimed byte-identical to the existing official firmware.

The camera DMA compatibility layer relies on Arduino 2.0.17 internals. TLS cancellation, cleanup, I/O, trust, certificate dates and DNS use linker wrappers. Upgrading the SDK requires focused device and host validation of those boundaries; do not patch installed caches.

Run the checks in [CONTRIBUTING](../CONTRIBUTING.md). The curated draft does not include the private repository’s entire host/device test suite, manufacturing tools or CI. Those continue to live in the private development repository.

The new workflow is manual and runs only from main. Private draft builds do not require public-launch approval. Its builds are custom production-policy candidates, and releases always start as drafts. It does not sign OTA manifests, enroll devices, publish cloud channels, or access the private source repository.

## Public shipping acceptance

Run `python3 tools/test_manufacturing.py`, `python3 tools/test_factory_test.py`,
and `python3 tools/test_device_handoff.py` on a host with a C++17 compiler and
AddressSanitizer/UndefinedBehaviorSanitizer. Signed Public OTA download completion
has distinct image-bound evidence (`online=3`). Only that proof qualifies Public
shipping; private channels retain credential verification. Cleanup supports
credential-free Public devices while preserving calibration, protected assets,
and manufacturing evidence. First-boot sound-effects volume is 100%; the speaker
gain ceiling remains 20%. Existing saved volume preferences are retained.

After verified cleanup, telemetry collection stays disabled across the verification
reboot until physical shipping unlock. This prevents boot/tutorial usage events
from recreating personal-data files before packing. The inventory check still
rejects any nonempty telemetry journal; no file is silently exempted.
