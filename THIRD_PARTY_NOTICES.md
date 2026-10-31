# Third-party components and review status

MIT at the repository root applies to approved Origami-original work only. Retained source notices remain authoritative. This inventory is preliminary, not a complete license clearance or SBOM.

| Component | Evidence retained | Review status |
| --- | --- | --- |
| Espressif esp_new_jpeg 1.0.2 | `lib/esp_new_jpeg/LICENSE`, pinned source and binary hashes in `lib/esp_new_jpeg/provenance.json` | Espressif MIT notice retained; use restricted to Espressif products; target is ESP32-S3 |
| Lua 5.4.9 | `lib/lua/LICENSE` | MIT notice retained |
| minimp3 | `lib/minimp3/LICENSE` | CC0 notice retained |
| qrcodegen | `lib/qrcodegen/LICENSE` | MIT notice retained |
| quirc 1.2 | License headers in `lib/quirc` | ISC-style permission retained |
| TFT_eSPI fork | `lib/TB_TFT_eSPI/license.txt`, font licenses and source headers | Mixed upstream notices; font-by-font coverage still requires review |
| OV5640 autofocus loader/blob | Headers in `lib/ESP32_OV5640_AF` | Public-domain assertion present; independently confirm blob redistribution rights |
| Chromium dinosaur artwork | `tools/trex-style-preview/CHROMIUM-LICENSE` and generated header attribution | BSD notice retained |
| Mozilla CA extract | `certs/mozilla.json`, `certs/mozilla.pem`, `certs/README.md` | MPL-2.0 provenance recorded; review distribution notice obligations |
| Generated Instrument font and Origami art | Compiled headers and source | Origami ownership confirmed by the requester on 21 October 2026; original work covered by MIT |
| PlatformIO/Arduino and downloaded libraries | Exact dependencies in `platformio.ini` | Complete resolved dependency license inventory and binary notices before distribution |

The commercial MP3, source design studies, vendor font-generation tool assets and private diagnostics were not imported. Only the two required owner-confirmed SD artwork files were subsequently added. Exclusion from this draft does not change or delete them in the private repository.

## Resolved build dependencies

The resolved build inventory is in `third_party/dependencies.json`; available library-level license files are retained under `third_party/licenses/`. WebSockets 2.7.3 declares LGPL-2.1. Review its binary-distribution obligations and the full Arduino/SDK dependency closure before publishing any application binary. Missing notice files or manifest license fields are unresolved evidence gaps, not permission to redistribute.

Arduino-ESP32 also declares LGPL-2.1-or-later. The STM32duino LSM6DSL source permission header is now retained. See `docs/distribution.md` for the source/relink approach and the remaining precompiled SDK evidence gap.
