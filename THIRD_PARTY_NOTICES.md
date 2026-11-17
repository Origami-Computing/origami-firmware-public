# Third-party components and review status

MIT at the repository root applies to approved Origami-original work only. Retained source notices remain authoritative. This inventory is preliminary, not a complete license clearance or SBOM.

| Component | Evidence retained | Review status |
| --- | --- | --- |
| Espressif esp_new_jpeg 1.0.2 | `lib/esp_new_jpeg/LICENSE`, pinned source and binary hashes in `lib/esp_new_jpeg/provenance.json` | Espressif MIT notice retained; use restricted to Espressif products; target is ESP32-S3 |
| Lua 5.4.9 | `lib/lua/LICENSE` | MIT notice retained |
| Espressif elf_loader 2.1.0 | `lib/esp_elf_loader/license.txt`, `lib/esp_elf_loader/UPSTREAM.md` (pinned commit, local atomic-builtin change) | Apache-2.0 notice retained; only signed Kami app packages are loaded |
| ggwave 0.4.3 | `lib/ggwave/LICENSE`, `lib/ggwave/src/reed-solomon/LICENSE`, `lib/ggwave/README.origami.md` (local PSRAM arena patch) | MIT notices retained |
| wpa_supplicant SAE (ESP-IDF v4.4.7) | `vendor/wpa3-sae/README`, `vendor/wpa3-sae/upstream-sha256.json`, `vendor/wpa3-sae/ORIGAMI.md` (leak correction) | BSD-3-Clause notice retained |
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

# Cine camera

The camera and gallery improvements incorporate code from thesephist/kami-os,
commit 18a67eea, under the license below.

MIT License

Copyright (c) 2026 Origami Firmware contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## Cine Gold and Portra matrix data

The fitted Gold/Portra data in `include/core/cine_film_matrices.h` derives from
**spektrafilm by Andrea Volpato**, https://github.com/andreavolpato/spektrafilm,
commit `3bb2c2d2801ff68b92019cf1dbcbb133d60832bc`. This data remains
**CC BY-SA 4.0** and is excluded from the repository's default MIT grant.
See [the unmodified upstream license](third_party/licenses/spektrafilm/SPEKTRAFILM_LICENSE.txt)
and [modification record](third_party/licenses/spektrafilm/CHANGELOG.txt).
The firmware uses fitted matrix constants; it does not include the simulator code.
