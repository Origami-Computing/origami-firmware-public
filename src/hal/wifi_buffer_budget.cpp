#include <esp_arduino_version.h>
#include <esp_wifi.h>
#include "config/memory_budget.h"

// Arduino's dynamic profile keeps four RX buffers but allocates outgoing DMA
// packets from the same fragmented internal heap as audio and TLS AES staging.
// A count limit alone did not reserve those bytes: live voice exhausted both
// AES staging and WiFi's esf_buf_alloc_dynamic. Reserve four TX buffers at radio
// initialization instead. Keep Arduino's smaller RX/cache profile; selecting
// its full static profile would reserve eight RX and eight TX buffers.
// This SDK boundary applies on every radio reinitialization, including Camera.
// Keep the SDK cache unmodified; revalidate on framework upgrades.
static_assert(ESP_ARDUINO_VERSION == ESP_ARDUINO_VERSION_VAL(2, 0, 17),
              "Revalidate WiFi buffer budgeting against the upgraded Arduino core");

extern "C" esp_err_t __real_esp_wifi_init(const wifi_init_config_t* config);
extern "C" esp_err_t __wrap_esp_wifi_init(const wifi_init_config_t* config) {
    if (!config) return __real_esp_wifi_init(config);
    wifi_init_config_t bounded = *config;
    if (bounded.tx_buf_type == 1 && bounded.dynamic_tx_buf_num > 0) {
        bounded.tx_buf_type = 0;
        bounded.static_tx_buf_num = bounded.dynamic_tx_buf_num < cfg::memory::kWifiTxBuffers
            ? bounded.dynamic_tx_buf_num : cfg::memory::kWifiTxBuffers;
        bounded.dynamic_tx_buf_num = 0;
    }
    return __real_esp_wifi_init(&bounded);
}
