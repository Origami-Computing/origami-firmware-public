#include "hal/hal_boot.h"

#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

namespace hal {
namespace boot {

static esp_partition_subtype_t subtypeFor(App app) {
  switch (app) {
    case App::Menu:
      return ESP_PARTITION_SUBTYPE_APP_OTA_0;
    case App::Mp3:
      return ESP_PARTITION_SUBTYPE_APP_OTA_1;
    default:
      return ESP_PARTITION_SUBTYPE_APP_OTA_0;
  }
}

bool setBootApp(App app) {
  esp_partition_subtype_t subtype = subtypeFor(app);
  const esp_partition_t* part =
      esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, nullptr);
  if (!part) {
    return false;
  }
  uint8_t magic = 0;
  if (esp_partition_read(part, 0, &magic, sizeof(magic)) != ESP_OK) {
    return false;
  }
  if (magic != 0xE9) {
    return false;
  }
  return esp_ota_set_boot_partition(part) == ESP_OK;
}

void rebootTo(App app) {
  if (setBootApp(app)) {
    delay(50);
    ESP.restart();
  }
}

}  // namespace boot
}  // namespace hal
