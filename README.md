<p align="center">
  <img src="docs/assets/origami-computing.svg" alt="Origami Computing" width="88" height="98">
</p>
<h1 align="center">kamiOS 1.0</h1>
<p align="center"><strong>Origami Computing</strong></p>

kamiOS is the operating firmware for Origami’s ESP32-S3 handheld: a display, camera, audio, motion sensing, and Lua widgets in one device.

Official releases are published after source review, automated checks, and qualification of the signed candidate. See [release review](docs/release-review.md).

## Build your own app

Describe an idea to your coding assistant and give it the [Origami app builder skill](skills/origami-app-builder/SKILL.md). It builds native C++ apps in this repository, registers them in the device menu, and keeps a persistent custom-project workspace for briefs, assets and test notes. It includes [native development](skills/origami-app-builder/references/cpp.md) and [USB installation/recovery](skills/origami-app-builder/references/usb-deployment.md) instructions. Existing Lua apps remain supported; new-app guidance uses native C++.

Read the skill directly from the checkout, or copy the entire `skills/origami-app-builder` folder into your assistant's skills directory. Keep its references together and point the assistant to the actual firmware checkout.

Try: “Use the Origami app builder skill to make a breathing app with a gentle animation and rotary speed control.”

## Hardware

The supported target is rev-4 ESP32-S3 N16R8: 16 MB flash, 8 MB octal PSRAM, a 240 × 280 ST7789 display, and a TCA6424A I/O expander. Firmware for other boards is not interchangeable.

## Build

Install PlatformIO Core 6.2.0 in a dedicated Python environment, then run:

```sh
pio run -e esp32-s3-menu-n16r8-production
```

This produces a **custom production-policy build**, not an official signed OTA release. The pinned Arduino framework, libraries, and compatibility wrappers are part of the build contract. See [building](docs/building.md).

## Firmware

This source targets **v1.0.6**. It adds a reworked onboarding flow and Wi-Fi setup by sound, an App Store that installs signed native apps by QR or six-digit code, IMU calibration, a clock that survives deep sleep, battery sleep with alarm wake, faster wake to Home, automatic upload of saved recordings, and memory and network recovery fixes. Native app packages load only with a valid publisher signature and matching hashes.

Earlier releases added on-device video recording with audio and local playback within Photos (v1.0.4) and Public updates without an OTA enrollment key (v1.0.5).

Run `python3 tools/test_mp3_metadata.py` and `python3 tools/test_playback_start_admission.py` for the metadata, decoder, seeking and failed-start regression checks.

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

Public updates since v1.0.5 require no OTA enrollment key. HTTPS, signed manifests, image hashes, hardware/layout checks and update confirmation remain mandatory. Private and Staging retain developer authorization. This change only takes effect after installation; legacy recovery is qualified separately for each supported updater and partition layout.
