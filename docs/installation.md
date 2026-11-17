# Installation and recovery

The application image targets rev-4 ESP32-S3 N16R8, 16 MB flash and 8 MB octal PSRAM, with the `n16r8-ota-v1` layout. It is not a factory installer. Installation on other boards or partition layouts is unsupported.

## SD card

Copy `sd_flash/eyes/` and `sd_flash/boot/` into the root of an existing FAT32 card, preserving all other files. Check the hashes in `sd_flash/asset-manifest.json`. These owner-confirmed assets supply the default home eyes and boot logo. Optional media and widgets are not included.

## Existing provisioned device

Use **Update Firmware** on the device and confirm the offered Public update. Public firmware since v1.0.5 needs no OTA enrollment key; Private and Staging remain restricted. Backend registration is separate. An application update preserves the existing partition layout, settings and SD files. See the [flash and update guide](../README.md#how-ota-works).

## OTA failure: guided recovery

Start at **[Kami recovery](https://origami-computing.github.io/origami-firmware-public/)** using Chrome or Edge on a computer and a USB data cable. Close other device tools, choose **Connect my Kami**, then **Install Public**, and wait for verification.

The page currently installs Public v1.0.5 on supported existing 16 MB devices. After restarting, connect to Wi-Fi and use the device updater to reach the latest Public release without an OTA enrollment key. This is a firmware recovery bridge, not a deleted-file recovery tool or factory installer.

The recovery page checks the installed layout and is designed to preserve pairing, Wi-Fi, settings and SD files. Unsupported layouts stop before writing. Follow the page's troubleshooting steps or contact support if it cannot proceed; do not substitute erase-all or a factory reset.

## USB recovery and new hardware

Do not write application-only firmware at address zero or assume `0x10000` is the active slot. Confirm the device hardware, partition table and selected OTA slot first. A recovery procedure must preserve NVS, SD data and the shipping partition. Merged images and erase-all can destroy settings and credentials.

The public application-only package cannot perform factory setup by itself. It deliberately contains neither keyed installers nor provisioning credentials. A matching component-based recovery package and approved provisioning procedure are required for new or damaged devices.

## Recovery validation and remaining acceptance

On a designated test device, record the baseline, test a normal update/reboot, verify Wi-Fi/settings/SD preservation, exercise camera/audio/input and the core UI, then test the documented recovery path. Keep any credential-bearing backups private. The source build and host checks do not replace these tests. A USB recovery round trip was completed on a designated N16R8 rev-4 device on 21 October 2026: the backed-up v0.1.200 application was restored to the confirmed active slot, booted, and then replaced with the reviewed custom v0.1.213 application. Both writes passed flash hash verification; both versions booted and mounted SD. Partition-table and OTA-selection bytes were unchanged. All 65 live NVS entries matched before and after, excluding obsolete records/page bookkeeping. The user confirmed normal operation before recovery; final physical confirmation after recovery remains pending. This validates application-only USB recovery for that existing layout, not factory provisioning, network OTA rollback, or every hardware configuration. The device used the older compatible table without a shipping partition; that table was preserved.
