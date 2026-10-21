#pragma once

/**
 * @file device_id.h
 * @brief Stable per-device identity derived from hardware.
 *
 * The device_id is the ESP32-S3 factory eFuse MAC formatted as
 * "esp32s3-AABBCCDDEEFF". It is:
 *   - globally unique (Espressif owns the MAC blocks),
 *   - stable across reboots, OTA updates, NVS wipes, and full reflashes
 *     (it is burned hardware, not stored state),
 *   - NOT a secret — observable/guessable, so it only NAMES the device.
 *     Trust comes from the per-device token (see core::secrets::deviceToken()).
 *
 * Computed once on first call and cached.
 */

namespace core {
namespace device {

/** Stable device id, e.g. "esp32s3-A4CF12FE5588". Never null, never empty. */
const char* id();

/** Firmware version string (OTA_FIRMWARE_VERSION). Never null. */
const char* firmwareVersion();

}  // namespace device
}  // namespace core
