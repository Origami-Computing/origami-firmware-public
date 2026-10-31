# Pinned ESP32-S3 JPEG decoder

This is the unmodified decoder interface and ESP32-S3 archive from Espressif's
esp_new_jpeg 1.0.2. Exact upstream revision and file hashes are in
[provenance.json](provenance.json); retain [LICENSE](LICENSE) when distributing it.
The license permits use on Espressif products.

PlatformIO discovers the public headers and links the local archive through
[library.json](library.json). No build-time download or SDK cache patch is used.
Only the decoder is called; no encoder task or alternate display path is added.

The firmware adapter lives in `src/core/preview_decoder.cpp`. Video uses unscaled
RGB565 big-endian MCU rows and an aligned 20 KiB PSRAM scratch buffer. The adapter
validates source geometry, output size and row count before mapping to the
existing letterboxed surface. QVGA camera preview uses row decoding with its centered crop. VGA camera
preview uses native scale/crop/rotation directly into an aligned surface,
without row scratch. JPEGDEC remains a preparation-allocation fallback. Native
header dimensions describe transformed output; JPEG SOF validation bounds the
source independently. Whole-frame cancellation is checked before/after the codec
call, while row mode checks between blocks.

Review binary/API compatibility, archive hashes, memory/stack use, malformed
input handling, rendered pixels and physical playback when upgrading. Host
adapter tests mock the vendor API and cannot establish binary decoder safety.
See `docs/build_reproducibility.md` and `docs/video_production.md` in the repository
for the pinned framework and current device acceptance limits.
