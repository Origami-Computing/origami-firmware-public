# Security hardening candidate

This patch requires review and deployment. It is not a statement that deployed
firmware or the live CDN already has these changes.

Production images do not open the legacy captive Wi-Fi portal or plaintext LAN
automation listener. QR/Bluetooth provisioning remains available. Authenticated
outbound automation polling remains; immediate local pushes no longer work and
new events can wait approximately one polling interval (60 seconds), or longer
when offline or another operation holds the network.

Widget IDs and directory names are bounded path segments. HTTP widget manifests
are limited to 16 KiB and 32 entries, Lua scripts to 32 KiB, data to 8 KiB, and
outbound automation snapshots to 16 KiB. These callers request HTTP/1.0 and reject
unknown-length or chunked bodies, truncated transfers and transfers exceeding
10 seconds. The backend must return Content-Length; verify that contract before
rollout. No credential-bearing redirect is followed in production.

Certificate bundle verification only permits a weak digest for an exact trusted
root (subject and public key). Weak child signatures and other certificate errors
remain fatal. Lua is updated to 5.4.9, retaining the limited library set and
32-bit number configuration, with saturated GC-step arithmetic. This does not
turn native Lua into a process-isolated security boundary. Physical secure boot
and flash encryption remain unchanged; no irreversible eFuse changes are made.

## Release access

| Credential | Public | Private | Staging |
| --- | --- | --- | --- |
| Production | Yes | No | No |
| Developer | Yes | Yes | Yes |
| Missing/invalid | No | No | No |

Public comes from `Origami-Computing/origami-firmware-public/main`; Private comes
from `Origami-Computing/origami_firmware/main`; Staging comes from that private
repository's `staging`. NVS channel 0 becomes Public, 1 remains Staging, and 2 is
Private. A developer credential can select all three even on a production-policy
image; this does not re-enable the legacy services. Selection, manifest retrieval,
download, signed paths and CDN permissions all enforce channel boundaries.

The old CDN `production` namespace contains private-source releases. It must be
developer-only after migration. Old production clients therefore need a reviewed
USB upgrade or an explicitly prepared signed public bridge; do not alias their
old private firmware path to Public. Apply this migration as a coordinated rollout,
not an isolated edge-function edit.

GitHub draft releases are hidden from ordinary readers. The publication workflow
requires exact-tree source/license/security approval before creating a visible
release and publishing its signed public OTA manifest. Public application binaries
must never contain access keys or NVS images. Keyed installers stay private.
