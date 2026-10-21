#include "core/device_id.h"

#include <Arduino.h>
#include <esp_mac.h>
#include <stdio.h>

#include "core/menu_config.h"  // OTA_FIRMWARE_VERSION

namespace core {
namespace device {

namespace {
char g_id[24] = {0};  // "esp32s3-" (8) + 12 hex + NUL = 21
}  // namespace

const char* id() {
  if (g_id[0]) return g_id;

  // Factory-burned base MAC (read-only). esp_efuse_mac_get_default() / the
  // ESP_MAC_WIFI_STA read both return the same 6 bytes used everywhere.
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(g_id, sizeof(g_id), "esp32s3-%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return g_id;
}

const char* firmwareVersion() {
#if defined(OTA_BUILD_OFFICIAL) && OTA_BUILD_OFFICIAL
  return OTA_FIRMWARE_VERSION;
#else
  return OTA_FIRMWARE_VERSION "-dev";
#endif
}

}  // namespace device
}  // namespace core
