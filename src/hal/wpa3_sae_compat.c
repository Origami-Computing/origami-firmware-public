// Source-level replacement of pinned IDF 4.4.7 SAE; see vendor/wpa3-sae/ORIGAMI.md.
#include <esp_idf_version.h>
#include <esp_arduino_version.h>
#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(4, 4, 7) || ESP_ARDUINO_VERSION != ESP_ARDUINO_VERSION_VAL(2, 0, 17)
#error Revalidate the WPA3 SAE cleanup replacement for the new SDK
#endif
// Same component-local definitions as the upstream WPA supplicant CMake target.
#define __ets__
#define ESP_SUPPLICANT
#define IEEE8021X_EAPOL
#define EAP_PEER_METHOD
#define EAP_MSCHAPv2
#define EAP_TTLS
#define EAP_TLS
#define EAP_PEAP
#define EAP_FAST
#define USE_WPA2_TASK
#define CONFIG_WPS2
#define CONFIG_WPS_PIN
#define USE_WPS_TASK
#define ESPRESSIF_USE
#define ESP32_WORKAROUND
#define CONFIG_ECC
#define CONFIG_IEEE80211W
#define CONFIG_SHA256
#define CONFIG_WNM
#define CONFIG_FAST_PBKDF2
#include <sdkconfig.h>
#if CONFIG_ESP32_WIFI_ENABLE_WPA3_SAE
#define CONFIG_WPA3_SAE
#endif
#if CONFIG_WPA_WPS_STRICT
#define CONFIG_WPS_STRICT
#endif
#if CONFIG_WPA_SUITE_B_192
#define CONFIG_SUITEB192
#endif
#if CONFIG_WPA_SUITE_B
#define CONFIG_SUITEB
#endif
#if CONFIG_ESP_WIFI_GCMP_SUPPORT
#define CONFIG_GCMP
#endif
#if CONFIG_ESP_WIFI_GMAC_SUPPORT
#define CONFIG_GMAC
#endif
#if CONFIG_WPA_MBO_SUPPORT
#define CONFIG_MBO
#endif
#if CONFIG_WPA_DPP_SUPPORT
#define CONFIG_DPP
#endif
#include "../../vendor/wpa3-sae/src/common/sae.c"
