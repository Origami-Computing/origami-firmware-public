# Installation and recovery

The application image targets rev-4 ESP32-S3 N16R8, 16 MB flash and 8 MB octal PSRAM, with the `n16r8-ota-v1` layout. It is not a factory installer. Installation on other boards or partition layouts is unsupported.

## SD card

Copy `sd_flash/eyes/` and `sd_flash/boot/` into the root of an existing FAT32 card, preserving all other files. Check the hashes in `sd_flash/asset-manifest.json`. These owner-confirmed assets supply the default home eyes and boot logo. Optional media and widgets are not included.

## Existing provisioned device

Use the existing on-device production update flow when an update is offered. An OTA access key remains separately provisioned; downloading the public application does not grant backend or OTA access. Do not publish keys to make setup easier.

## USB recovery and new hardware

Do not write application-only firmware at address zero or assume `0x10000` is the active slot. Confirm the device hardware, partition table and selected OTA slot first. A recovery procedure must preserve NVS, SD data and the shipping partition. Merged images and erase-all can destroy settings and credentials.

The public application-only package cannot perform factory setup by itself. It deliberately contains neither keyed installers nor provisioning credentials. A matching component-based recovery package and approved provisioning procedure are required for new or damaged devices.

## Hardware acceptance still required

On a designated test device, record the baseline, test a normal update/reboot, verify Wi-Fi/settings/SD preservation, exercise camera/audio/input and the core UI, then test the documented recovery path. Keep any credential-bearing backups private. The source build and host checks do not replace these tests. No device was flashed or reset during this migration.
