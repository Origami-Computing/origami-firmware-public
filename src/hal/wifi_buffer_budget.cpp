#include <esp_arduino_version.h>
#include <esp_wifi.h>

// Arduino's dynamic-buffer mode unconditionally requests 32 TX buffers. During
// sustained file uploads those buffers compete with audio DMA and hardware AES
// for internal RAM. Keep its four static RX buffers and on-demand TX allocation,
// but bound the TX pool to the eight buffers used by the pinned static profile.
// This boundary applies on every radio reinitialization, including wake/QR exit.
// Keep the SDK cache unmodified; revalidate this wrapper on framework upgrades.
static_assert(ESP_ARDUINO_VERSION == ESP_ARDUINO_VERSION_VAL(2, 0, 17),
              "Revalidate WiFi buffer budgeting against the upgraded Arduino core");

extern "C" esp_err_t __real_esp_wifi_init(const wifi_init_config_t* config);
extern "C" esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t* config) {
    if (!config) return __real_esp_wifi_init(config);
    wifi_init_config_t bounded = *config;
    if (bounded.tx_buf_type == 1 && bounded.dynamic_tx_buf_num > 8)
        bounded.dynamic_tx_buf_num = 8;
    return __real_esp_wifi_init(&bounded);
}
