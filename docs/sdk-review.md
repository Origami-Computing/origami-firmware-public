# ESP32-S3 SDK review for kamiOS 1.0

Reviewed 27 October 2026. This engineering review applies to the n16r8 production target and its pinned dependency set. It classifies the supplied components and known advisory exposure; it is not a guarantee that the software has no undiscovered vulnerabilities. Hardware qualification and the approved release hash are separate records.

## Package identity and license result

Arduino-ESP32 is pinned to `3.20017.241212+sha.dcc1105b` (Arduino 2.0.17 / ESP-IDF 4.4.7). Its registry archive hash and 3,407-file comparison are recorded in `third_party/sdk-provenance.json`. The exact IDF source revision is `38eeba213aa695aabfd6d89aa9f5078dbe5a94c3`. The descriptor's `dirty` suffix is accounted for by the registry package comparison; installed SDK caches are not edited.

The production map selects objects from 83 archives. Every selected archive has an entry in `third_party/linked-library-licenses.json`. `tools/check_sdk_review.py` rejects newly selected archives, missing notices, or linkage to the original unrepaired TLS translation unit. Linker input names alone do not imply use: unused archives listed by the linker are excluded from the selected-object inventory.

The identified license obligations are addressed by the retained notices and source/relink package:

| Component group | Classification and distribution treatment |
| --- | --- |
| Origami application, owner-confirmed artwork/Instrument font | MIT; separate from third-party ownership |
| Arduino core/libraries and WebSockets | LGPL-2.1-or-later and per-file exceptions; application/library source and rebuild/relink inputs accompany firmware; no prohibition on reverse engineering for debugging library modifications |
| ESP-IDF platform and Espressif Wi-Fi/PHY/Bluetooth controller libraries | Apache-2.0, plus retained component exceptions; vendor radio/controller implementations are distributed as binaries, not represented as available source |
| MbedTLS | Apache-2.0 option selected; upstream license, pinned source identity, modification notices and vendored repaired translation unit retained |
| FreeRTOS, cJSON, SPIFFS, QR, autofocus, LittleFS port | MIT/permissive component notices retained |
| lwIP, protobuf-c, LittleFS core, camera quirc, FatFS | BSD/ISC/ChaN permissive notices, including their attribution and disclaimer requirements |
| Newlib and Xtensa HAL | Per-file permissive terms; complete Newlib and Cadence notices retained |
| GCC runtime/libstdc++ | GPL with GCC Runtime Library Exception 3.1; the exception governs the embedded runtime, not a relicensing of the application |
| Optional GNU FreeFont bitmap headers | Their own GPL/font exception retained, with editable upstream font source; no FreeFont glyph symbols selected in the inspected production map |
| Unused SDK components/tests/examples | Not evidence of runtime exposure; unused precompiled archives excluded from the companion, example/test sources not imported into the application |

Additional closure: the installed LittleFS header matches upstream v1.14.1 byte-for-byte. Its MIT port and the pinned nested littlefs source notice are retained. Asio's Boost notice, the Cadence HAL notice, 779 distinct SDK header notices and 618 distinct IDF source notices supplement root licenses. Exact added notice URLs/hashes are in `third_party/additional-notice-sources.json`. The SDK source-notice collection intentionally includes unused component notices, preserving exceptions without claiming those components are linked.

No incompatible redistribution requirement was identified in this selected production archive set. This conclusion depends on shipping the companion and notices, preserving modifications/attribution, and rerunning the inventory when dependencies change. It does not make closed vendor radio implementation source available, relicense upstream components as MIT, or clear a different historical binary.

## TLS repairs and active security boundary

The pinned MbedTLS source is 2.28.7. `third_party/mbedtls-finished` rebuilds its TLS translation unit in the application:

- CVE-2025-27810: Finished hash/PRF failures propagate; Finished read/write stops without advancing state.
- CVE-2026-50581: failed transcript hashing aborts Extended Master Secret derivation before the PRF. SHA-256, SHA-384 and legacy TLS callbacks preserve the SDK callback ABI and signal failure with zero output length.
- `tests/tls_finished.c` exercises the real translation unit with hash/PRF fault injection under address/undefined-behavior sanitizers. The link-map check verifies the repaired object is selected and `libmbedtls_2.a(ssl_tls.c.obj)` is not.

The application uses outbound certificate-authenticated TLS and trusted roots. Hostname verification, certificate-chain validation, signed OTA manifests, image hashes and hardware/layout checks remain enabled. OTA bearer keys are download authorization, not TLS pre-shared keys. The production LAN HTTP listener and legacy setup AP are disabled. Bluetooth setup uses the application's NimBLE path, not IDF's protocomm BLE provisioning server.

## Configuration-specific advisory disposition

Primary references: [MbedTLS advisory index](https://mbed-tls.readthedocs.io/en/latest/security-advisories/) and [Espressif advisories](https://github.com/espressif/esp-idf/security/advisories). These dispositions concern the present application, not a claim that all code in the old SDK is patched.

| Advisory/family | Disposition for this target |
| --- | --- |
| CVE-2025-27810; CVE-2026-50581 | Repaired and fault-tested as above |
| CVE-2025-27809 (missing hostname) | The Arduino secure-client path sets the expected hostname; CA and hostname verification must remain required |
| CVE-2026-49300 (invalid CA bit) | No untrusted user-supplied trust-anchor registration/cross-parser root intake; immutable trusted-root bundle. Library issue remains upstream, outside the application's trust-input path |
| CVE-2026-50580 (ECDHE-PSK identity overflow) | No application TLS PSK identity/key configuration; certificate-based clients |
| CVE-2026-50586 (NewSessionTicket disclosure) | Requires a TLS server ticket-write callback; application runs TLS clients, not a ticket-issuing TLS server |
| CVE-2026-50585 (context reset) | DTLS bad-MAC/SRTP reuse prerequisites absent; TLS stream clients |
| CVE-2026-54441 (certificate signature policy) | Certificate profile/root policy is enforced separately; handshake signature list is not relied on as a certificate-chain restriction |
| CVE-2026-35336 (ECDH output buffer) | Upstream identifies normal TLS use as unaffected; no direct application ECDH buffer API |
| CVE-2026-34872 (FFDH contributory behavior) | Upstream excludes TLS 1.2 use; no application raw FFDH protocol |
| CVE-2026-34877 (serialized TLS state) | Application does not load attacker-controlled serialized TLS sessions/contexts |
| CVE-2026-50584 (ChaCha20 counter wrap) | No application raw ChaCha API or single-key/nonce operation approaching 256 GB; TLS record boundaries apply |
| CVE-2026-50587, CVE-2025-54764 and private-RSA side channels | Device uses public-key verification, not RSA private-key generation/decryption/signing; firmware signing private key stays off-device |
| CVE-2026-54435 (ECC modp local side channel) | Requires precise local traces of repeated secret operations. No persistent application ECC signing key; physical/debug compromise is outside the remote-network claim and is not solved by this review |
| CVE-2025-66442 (compiler constant-time) | Published demonstrated compiler path differs from pinned GCC 8.4 Xtensa build; no assertion that every possible physical side channel is eliminated |
| CVE-2025-59438 / PKCS7 padding oracle | No exposed padded-cipher/PSA decryption API; ordinary TLS uses its record implementation |
| CVE-2025-52497 (encrypted PEM) | No attacker-supplied password-encrypted PEM/private-key input; trusted roots and public signing key only |
| PKCS7, LMS, certificate-name writing and opaque-key export APIs | No application import/verification/write endpoint for these formats; not the X.509 server-certificate parsing path |
| TLS 1.3, PSA driver and newer-version-only issues (2026-25832/25833/25834, 34873/34875/34876, 50579/50583/50588, 50640/50713, 54434, 73064/73096) | Introduced in newer MbedTLS branches or absent newer APIs/protocols; not this 2.28.7 TLS 1.2 target |
| CVE-2023-43615, CVE-2022-35409 and older session double-free | Fixes precede 2.28.7; weak RC4/null-cipher use is not enabled by the application |
| ESP-IDF TEE, P4 hardware JPEG and Bluetooth Classic A2DP/AVRCP/SDP/L2CAP advisories | Wrong target/feature: ESP32-S3, no TEE/P4 JPEG/Classic Bluetooth application |
| ESP32 controller invalid access address GHSA-8mg7-9qpg-p92v | Vendor explicitly excludes ESP32-S3 |
| ESP-IDF WPS, ESP-NOW, ESPTouch-v2, BluFi example | Application does not start those protocols/examples |
| IDF protocomm Security2/BLE lifetime advisories | Application does not start IDF protocomm BLE/WiFiProv; NimBLE setup has a separate owner |
| IDF HTTP WebSocket-server and DHCP-server advisories | No IDF WebSocket server; production setup AP/DHCP-server path disabled |
| IDF bootloader anti-rollback TOCTOU / Bluetooth Mesh older issue | Fixes included in 4.4.7 / 4.4.2 respectively; no claim that secure boot or flash encryption is enabled |

The pinned SDK remains old. Any addition of TLS servers, raw crypto, trust-anchor upload, certificate generation, serialized TLS restoration, WPS/ESPNOW/SmartConfig or vendor BLE provisioning invalidates the corresponding exclusions and requires a fresh review. Continue tracking upstream advisories and plan a separately qualified supported-SDK upgrade; avoid silently changing the framework beneath a qualified release.

## Acceptance evidence and limits

For each release retain the source digest, selected-library inventory, build/link-map checks, sanitizer results and hardware HTTPS/OTA results. A passing SDK review does not establish an installation, recovery, backend compatibility or legacy-client migration result. Those are recorded against the exact signed release candidate. Secure boot/flash encryption were not enabled as part of this migration; physical access to an unencrypted device can expose its stored credentials.
