# Firmware source and redistribution

Origami-original code and owner-confirmed art/font use MIT. The root license does not replace the retained licenses of libraries and third-party artwork.

## LGPL components

Both WebSockets 2.7.3 and Arduino-ESP32 2.0.17 include LGPL-2.1-or-later code. For the static firmware image, retain the full notices and provide the corresponding library source and application source/relink materials alongside the binary. Users may modify the libraries and rebuild; this project adds no restriction on reverse engineering for debugging such modifications.

This follows the source/relink route described in [LGPL 2.1 section 6](https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html). A top-level MIT license or checksum alone does not satisfy that route.

## Reproduction recipe

The ordinary default is a custom production-policy build. To reproduce the compile-time policy of the original production image:

```sh
pio run -e esp32-s3-menu-n16r8-release-review
```

This adds the official-build marker used by the production source. It does not sign or publish an OTA manifest, create access credentials or deploy anything. The build is a local reproduction candidate and must not be represented as the original official bytes. Existing platform/framework/library pins remain in effect. The public candidate includes reviewed security repairs and source updates; it is not the original private production image.

The release review contains a source companion archive with the tracked application tree, resolved direct-library sources and LGPL framework source. Standalone examples are omitted, including upstream WiFiClient demonstration keys; compiled library modules and license notices are retained. The companion also includes the exact ESP32-S3 SDK headers/precompiled objects needed for relinking and the editable GNU FreeFont source archive. No Git metadata, historical commits, credential stores, device dumps or private application build outputs are included. The companion excludes unused SDK binary archives and includes the selected, classified archive set.

## Vendor SDK evidence and review limits

The production workflow logs identify the exact pinned framework package. A fresh registry download passed its published SHA-256 checksum, and all 3,407 compared installed core/library/variant/ESP32-S3 SDK files match it. The seven differences against the upstream Git commit are package-level changes. The descriptor's `dirty` label is therefore not evidence of an unexplained local SDK patch.

The upstream ESP-IDF commit, 27 submodule references, available component notices and registry evidence are recorded in `third_party/sdk-provenance.json`. Root notices, additional LittleFS/Asio/Cadence notices, and source/header exception collections are retained; unused test/example components are identified separately. Original per-file license headers are retained in the supplied library sources.

The selected production archive set has now been classified in [SDK review](sdk-review.md) and `third_party/linked-library-licenses.json`. No incompatible redistribution requirement was identified for this set when accompanied by the notices and source/relink materials. This scope does not clear a different historical binary or claim access to vendor radio implementation source.

A successful source build establishes rebuildability of the candidate, not byte-for-byte identity with the existing image. The original Actions run still has logs but no retained ELF/link-map artifacts. Hardware installation/recovery qualification is separate.
