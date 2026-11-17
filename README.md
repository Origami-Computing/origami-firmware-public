<p align="center">
  <img src="docs/assets/origami-computing.svg" alt="Origami Computing" width="88" height="98">
</p>
<h1 align="center">kamiOS 1.0</h1>
<p align="center"><strong>Origami Computing</strong></p>

kamiOS is the operating firmware for Origami’s ESP32-S3 handheld: a display, camera, audio, motion sensing, and Lua widgets in one device.

Official releases are published after source review, automated checks, and qualification of the signed candidate. See [release review](docs/release-review.md).

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

## Updates and downloads

For a normal update, use **Update Firmware** on the device, review the offered version, and confirm installation over Wi-Fi. Read the [release log](CHANGELOG.md) for exactly what changed since the previous version.

For manual downloads, open the [latest release](https://github.com/Origami-Computing/origami-firmware-public/releases/latest):

| Download | Choose this when… |
| --- | --- |
| **firmware.bin** | You need the application image for an existing compatible device. |
| **kamiOS-vX.Y.Z.zip** | You want the complete release bundle: the same image, release notes, corresponding source, licenses and verification files. |

There is **one release ZIP**. GitHub's additional “Source code (zip)” and “Source code (tar.gz)” links are [automatic repository snapshots](https://docs.github.com/en/repositories/releasing-projects-on-github/about-releases), not install packages; you do not need them to update. The complete bundle is not a factory installer either. The source version is in `platformio.ini`; main may contain changes newer than the published release.

## Where firmware and custom apps live

The device has **16 MiB of flash** for persistent storage, **8 MiB of PSRAM** for working memory, and an SD card for media and apps. PSRAM is cleared on power-off; it is not extra storage.

| Flash area | Purpose |
| --- | --- |
| Firmware slot A — 6 MiB | One complete kamiOS image. |
| Firmware slot B — 6 MiB | A second image, so an update can be written without overwriting the running firmware. |
| Settings and system areas | Wi-Fi/settings, boot selection, crash information and shipping state. |
| Optional custom-app area — about 3.8 MiB | Separate persistent storage for native apps, **only on devices provisioned with the compatible `customapps` partition**. |

The [current partition table](partitions_ota_n16r8.csv) does **not** declare that optional app partition. The storage code supports it at `0xC30000` with size `0x3D0000`, but unused flash is not automatically an app filesystem. Native App Store downloads fall back to SD storage when that partition is absent. Enabling it needs a separately verified partition migration; installing a firmware update alone does not create it.

Custom apps do not consume the spare OTA slot. Lua widgets run from SD without rebuilding kamiOS. Signed native apps install separately through App Store and load into memory when opened; their ELF payload is currently limited to 512 KiB, with actual execution also limited by available memory and API compatibility. Adding C++ directly to the firmware is different: it increases the OS image, which must fit inside one 6 MiB slot. Photos, recordings and videos stay on SD.

## How OTA works

1. The device fetches update information over HTTPS and checks the signed manifest, version, expected image hash, hardware and partition-layout compatibility.
2. After you confirm, it downloads the image into the inactive firmware slot. The currently running image stays intact during the download.
3. The downloaded bytes are checked against the signed hash. Only a successfully validated image is selected for the next boot. The device then restarts into the update.

A normal OTA update replaces firmware only. It preserves settings, SD files and a separately provisioned custom-app partition; it does not resize partitions or install a bootloader. Keeping two images does not guarantee automatic recovery from every failure after reboot—recovery also depends on the installed bootloader and device state.

Public firmware updates since v1.0.5 need no OTA enrollment key. Private and Staging channels retain developer authorization. Backend account setup is separate from permission to download an OS update.

## If you need to flash over USB

Use USB for development or recovery when the normal updater cannot be used. **Do not write `firmware.bin` at address zero or erase the whole device.** It contains the OS application, not the bootloader, partition table or factory configuration.

1. Confirm the board is rev-4 ESP32-S3 N16R8 and inspect its installed partition table and selected boot slot. Do not guess the slot from the filename.
2. Back up the device privately, including settings and partition/boot information. Close other serial monitors and keep power stable.
3. Verify the download hash and write only the application image to the confirmed compatible application slot. The standard slot addresses are `0x10000` and `0x610000`; which one to use depends on the device's boot selection. Preserve all other flash regions.
4. Verify the written bytes, reboot, and confirm the version, settings, SD access and the affected features.

A blank device or a damaged partition table needs a matching recovery/factory package and provisioning procedure; this release bundle is not sufficient. See [installation and recovery](docs/installation.md) before writing anything. The included SD artwork lives in `sd_flash/` in the source companion.

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
