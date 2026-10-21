# Public TLS trust store

`mozilla.pem` is the unmodified Mozilla CA extract distributed by curl, dated
2026-09-25 (121 certificates). `mozilla.json` records its immutable download URL,
SHA-256 and MPL-2.0 license. The source remains available at that URL; license
terms are at https://www.mozilla.org/MPL/2.0/ .

Firmware embeds `mozilla.bundle`, the same roots in ESP-IDF bundle format, generated
with `python3 tools/gen_crt_bundle.py certs/mozilla.pem certs/mozilla.bundle`
(regenerate after every PEM update; `--check` fails when it is stale). Builds do not fetch
certificates from the network. Backend HTTPS and WSS clients reference the same
immutable store, while each TLS connection looks up only the root it needs. Existing
mbedTLS PSRAM allocation remains in effect. OTA and eye assets retain their
existing narrower Amazon anchors and signing policies.

To update: choose a dated extract from https://curl.se/docs/caextract.html,
download its PEM and published SHA-256 over HTTPS, verify the digest, replace the
PEM without editing it, regenerate the bundle, and update the metadata. Review additions/removals, build,
and repeat valid/untrusted/hostname/expiry tests before release. Updating the
firmware updates trust; it is not silently refreshed on device.

This PEM provides trust anchors, not Firefox's additional root constraints or
online revocation policy. Do not claim browser-equivalent certificate policy.
Local plain HTTP/WS remains an explicitly configured development transport;
failed HTTPS/WSS never downgrades to it.
