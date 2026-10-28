<p align="center">
  <img src="docs/assets/origami-computing.svg" alt="Origami Computing" width="88" height="98">
</p>
<h1 align="center">kamiOS 1.0</h1>
<p align="center"><strong>Origami Computing</strong></p>

kamiOS is the operating firmware for Origami’s ESP32-S3 handheld: a display, camera, audio, motion sensing, and Lua widgets in one device.

**Private review draft — not approved for public distribution.** This repository starts with one independent snapshot. Publication awaits the source, asset, dependency, and firmware review described in [release review](docs/release-review.md).

## Build your own app

Describe an idea to your coding assistant and give it the [Origami app builder skill](skills/origami-app-builder/SKILL.md). Start with Lua: small games, clocks, interactive tools and data screens can run from the SD card without rebuilding firmware. The skill includes a [ready-to-copy counter](skills/origami-app-builder/assets/counter/main.lua), the supported Lua API and C++ guidance for deeper hardware features.

With a skill-compatible assistant, copy the entire `skills/origami-app-builder` folder into its skills directory and invoke `$origami-app-builder`. In Codex, that directory is `~/.codex/skills/`. Other assistants can read `SKILL.md` and its linked references directly from this checkout.

Try: “Use the Origami app builder skill to make a breathing app with a gentle animation and rotary speed control.” Lua supports the device's exposed capabilities; arbitrary network, storage and hardware access need native integration.

## Hardware

The supported target is rev-4 ESP32-S3 N16R8: 16 MB flash, 8 MB octal PSRAM, a 240 × 280 ST7789 display, and a TCA6424A I/O expander. Firmware for other boards is not interchangeable.

## Build

Install PlatformIO Core 6.2.0 in a dedicated Python environment, then run:

```sh
pio run -e esp32-s3-menu-n16r8-production
```

This produces a **custom production-policy build**, not an official signed OTA release. The pinned Arduino framework, libraries, and compatibility wrappers are part of the build contract. See [building](docs/building.md).

## Firmware

The upcoming release is **kamiOS 1.0** (`v1.0.1`). This patch version includes reliable interrupted-download recovery and upgrades the earlier `v1.0.0` qualification image. Qualification builds remain private candidates until the release review passes. Earlier `v0.1.213` drafts are historical and must not be published as kamiOS 1.0. Use its SHA-256 manifest to identify the reviewed bytes. No public release is available yet.

`firmware.bin` is an application image, not a full-device installer. Do not write it at address zero. [Installation notes](docs/installation.md) explain the current review boundary. Credentials and OTA access must be provisioned separately; they are never distributed with this repository. The minimal owner-confirmed SD artwork is included under `sd_flash/`.

## Explore

- [Architecture and ownership](docs/architecture.md)
- [Build and verification](docs/building.md)
- [SDK license and security review](docs/sdk-review.md)
- [Private-to-public promotion runbook](docs/public-promotion.md)
- [Contribution guide](CONTRIBUTING.md)
- [Security reporting](SECURITY.md)
- [Third-party notices](THIRD_PARTY_NOTICES.md)

## License

Origami’s original source is offered under [MIT](LICENSE), as selected for this migration. Existing third-party licenses continue to govern their respective components. Origami confirmed ownership of its artwork and generated font for this migration. Upstream components retain their own licenses; see [distribution requirements](docs/distribution.md).
