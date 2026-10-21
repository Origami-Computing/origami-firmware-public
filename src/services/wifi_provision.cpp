#include "services/wifi_provision.h"
#include <Arduino.h>
#include <WiFi.h>
#include <cstring>
#include "core/secrets.h"
#include "hal/hal_wifi.h"

namespace services { namespace wifi_provision {
bool saveAndConnect(const Credentials& creds, bool keepApUp) {
  if (!creds.ssid[0]) return false;

  if (!core::secrets::addWifiNetwork(creds.ssid, creds.pass)) {
    Serial.println("[wifi_provision] NVS add failed (full?)");
    return false;
  }
  (void)core::secrets::promoteWifiNetworkToFront(creds.ssid);
  (void)core::secrets::setLastConnectedWifi(creds.ssid);

  if (creds.hasApiKey && creds.apiKey[0]) {
    if (!core::secrets::setApiKey(creds.apiKey)) {
      Serial.println("[wifi_provision] API key save failed");
    }
  }

  // Defer the radio mode-change + join to the wifi_mgr task (Core 0). Calling
  // WiFi.mode()/begin() inline here runs on the UI core (Core 1), where — after
  // the WiFi-off-UI-core refactor — WiFi.mode() returns false and leaves the
  // radio uninitialized (wl=255), so the QR/portal join silently never happens.
  // requestConnect() routes these blocking calls to Core 0 like the boot path.
  //
  // keepApUp (web-portal path): join in APSTA so the soft-AP stays reachable —
  // the phone is waiting on it for the /connect response; dropping it would make
  // the page show "Request failed" even though creds were saved. The AP is torn
  // down later in wifi_portal_app::exitApp(). Otherwise join in plain STA.
  hal::wifi::requestConnect(keepApUp ? WIFI_MODE_APSTA : WIFI_MODE_STA);
  return true;
}

bool connectSaved(const char* ssid) {
  if (!ssid || !ssid[0]) return false;
  // Settings passes a pointer into the saved list, which promotion reorders.
  char selected[sizeof(Credentials{}.ssid)];
  const size_t length = strnlen(ssid, sizeof(selected));
  if (length == sizeof(selected)) return false;
  memcpy(selected, ssid, length + 1);
  // Make the chosen network the first the backoff tries.
  (void)core::secrets::promoteWifiNetworkToFront(selected);
  (void)core::secrets::setLastConnectedWifi(selected);
  // Defer the radio mode-change + join to the wifi_mgr task (Core 0). Same path
  // as the QR/portal save flow; calling WiFi.mode()/begin() inline on the UI
  // core fails (esp_wifi_init 257).
  hal::wifi::requestConnect(WIFI_MODE_STA);
  return true;
}

bool forgetSaved(int index) {
  if (!core::secrets::removeWifiNetwork(index)) return false;
  // Cache/radio reconciliation belongs to the Core-0 manager, including after
  // removal of the connected network. Retain the existing success-only policy.
  hal::wifi::requestCredentialsChanged();
  return true;
}

} }
