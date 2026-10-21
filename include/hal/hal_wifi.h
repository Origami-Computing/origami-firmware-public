#pragma once

#include <Arduino.h>
#include <esp_wifi.h>
#include "core/wifi_qr_parser.h"

// WiFi HAL - Persistent WiFi layer using Arduino WiFi library
// Supports both Station (STA) and Access Point (AP) modes
// Credentials stored in NVS using Preferences

namespace hal {
namespace wifi {

/**
 * High-level ownership of the shared Wi-Fi/BLE radio lifecycle.
 *
 * This is intentionally tiny (one byte) and describes transition safety, not
 * merely WiFi.status(). In particular, RestoringWifi means BLE must stay down
 * even though STA is not connected yet.
 */
enum class RadioState : uint8_t {
  StartingWifi,
  WifiOwned,
  Suspended,
  RestoringWifi,
  OfflineBle,
};

RadioState radioState();
const char* radioStateName();

// Initialize WiFi subsystem (call once in setup)
// This initializes NVS, netif, event loop, and WiFi driver
bool init();

/**
 * Apply modem sleep policy for Wi-Fi/BLE coexistence. When NimBLE is running, ESP-IDF requires
 * modem sleep enabled — disabling it (`WiFi.setSleep(false)`) can abort in `pm_set_sleep_type`.
 * Call after mode/AP/STA configuration changes (same places `WiFi.setSleep` was used).
 */
void applyModemSleepPolicy();

// Set WiFi mode (STA, AP, or APSTA)
bool setMode(wifi_mode_t mode);

// Get current WiFi mode
wifi_mode_t getMode();

// Station (STA) mode functions
//
// The HAL keeps an in-RAM list of up to `kMaxStaCredentials` known networks.
// `connect()` issues a non-blocking `WiFi.begin` for the first (or
// last-connected) credential; `tickJoinBackoff()` round-robins the rest on
// timeout — no synchronous full-channel scans on the UI thread.
constexpr int kMaxStaCredentials = 4;

/** Append (or in-place update if SSID matches) one STA credential. Returns
 *  false if the table is already full or `ssid` is empty. Does NOT trigger a
 *  connect — call `connect()` (or wait for `tickJoinBackoff`) to apply. */
bool addSTACredential(const char* ssid, const char* password);

/** Wipe every registered STA credential. Useful before re-pushing the secrets
 *  cache after a remove. */
void clearSTACredentials();

/** Number of currently registered STA credentials (0..kMaxStaCredentials). */
int staCredentialCount();

/** Legacy single-credential setter: equivalent to `clearSTACredentials()` +
 *  `addSTACredential(ssid, password)`. Kept for callers that only set one. */
bool setSTACredentials(const char* ssid, const char* password);

/** Rebuild the in-RAM STA list from `core::secrets` (after NVS promote/remove). */
void reloadCredentialsFromSecrets();

/** Request a saved-credential reconnect on wifi_mgr, safe from UI/voice tasks.
 *  Returns whether the intent was accepted, not whether association succeeded.
 *  Repeated requests preserve an ongoing join and intentional radio suspension;
 *  tickJoinBackoff() owns rotation/timeouts. Use requestConnect(mode) when an
 *  explicit provisioning action needs to change mode or replace a join. */
bool connect();
/**
 * Call from main loop: if STA is not connected, stop aggressive rejoin after HAL_WIFI_STA_JOIN_GIVEUP_MS
 * (default 20 s, overridable via -DHAL_WIFI_STA_JOIN_GIVEUP_MS=...), then rare probes every
 * HAL_WIFI_STA_PROBE_INTERVAL_MS (default 90 s).
 */
void tickJoinBackoff();
void disconnect();        // Disconnect from AP
bool connected();         // Check if connected to AP
// Read-only join activity for shared UI. False during deliberate suspension or retry backoff.
bool connecting();
bool suspended();         // True while WiFi is deliberately down for camera/photos

/** Radio preference hook. BLE-over-WiFi preference is disabled for now; calling
 *  setPreferBle(true) is ignored and clears the persisted setting. */
void setPreferBle(bool on);
bool preferBle();
esp_ip4_addr_t getSTAIP(); // Get station IP address

// Access Point (AP) mode functions
bool setAPCredentials(const char* ssid, const char* password, uint8_t channel = 1, uint8_t maxConnections = 4);
bool startAP();           // Start soft AP
void stopAP();            // Stop soft AP
bool apStarted();         // Check if AP is running
esp_ip4_addr_t getAPIP(); // Get AP IP address
uint8_t getAPClientCount(); // Get number of connected clients

// Scanning (STA mode)
bool scan(wifi_ap_record_t* results, uint16_t* count, uint16_t maxResults);

// Stop WiFi temporarily (remembers state for restore)
void stop();

/**
 * Explicit driver-deinit attempt plus an 80 ms settling delay, preserving the
 * high-resolution camera policy. In the pinned Arduino 2.0.17 implementation,
 * WIFI_OFF already reaches esp_wifi_deinit(); plain stop() can release driver
 * pools too. Do not assume a distinct RAM saving or restore-time advantage
 * without measuring this build. Safe to call after stop().
 */
void powerDown();

// Restore WiFi to previous state after stop()
bool restore();

/** After stop(), restore should usually happen asynchronously so UI (menu/scroll) isn't blocked on STA join. */
void scheduleRestore();
/** Call from main loop: performs a pending restore created by scheduleRestore(). */
void servicePendingRestore();

/**
 * Request a mode change + STA join, deferred to the wifi_mgr task (Core 0).
 * Safe to call from any core (e.g. provisioning on the UI core). The blocking
 * WiFi.mode()/begin() calls must run on Core 0 or they fail (wl=255).
 */
void requestConnect(wifi_mode_t mode);
/** wifi_mgr (Core 0) only: perform a pending requestConnect(). */
void servicePendingConnect();

/**
 * Notify wifi_mgr that core::secrets credentials changed. Safe from any core.
 * The manager performs the HAL cache reload and any required radio transition
 * on Core 0; callers must not call reloadCredentialsFromSecrets(), disconnect(),
 * or connect() directly afterward.
 */
void requestCredentialsChanged();
/** wifi_mgr (Core 0) only: consume requestCredentialsChanged(). */
void servicePendingCredentialsChanged();

/**
 * While true, `servicePendingRestore()` is a no-op (pending flag is kept).
 * Use during captive-portal WiFi setup (APSTA) so a deferred post-camera
 * restore cannot tear down the soft-AP on the next main-loop iteration.
 */
void setWifiRestoreDeferred(bool defer);

// Shutdown WiFi completely
void deinit();

// Get SSID (for display purposes)
const char* getSTASSID();
const char* getAPSSID();

// ---------------------------------------------------------------------------
// WiFi manager task (Option B)
//
// startTask() spawns a Core-0 FreeRTOS task that owns the WiFi boot sequence
// (init + setMode + connect + BLE init) and runs tickJoinBackoff /
// servicePendingRestore every 500 ms. Call once from services::init() — after
// core::secrets::init() has loaded credentials but before loop() starts.
//
// taskBooted() returns true once the boot sequence has completed. The UI loop
// does not need to gate on this — connected() stays false until the join
// succeeds regardless of who initiated it.
// ---------------------------------------------------------------------------
void startTask();
bool taskBooted();

// QR setup owns the radio from scanner preparation through result dismissal.
// All commands are asynchronous; only wifi_mgr touches the radio or saves credentials.
enum class QrPhase : uint8_t { Preparing, Scanning, Connecting, Joined, Failed };
enum class QrError : uint8_t { None, Radio, NetworkNotFound, Authentication, Timeout,
                              NoIp, NetworksFull, SaveFailed };
struct QrStatus {
  uint32_t id = 0;
  QrPhase phase = QrPhase::Preparing;
  QrError error = QrError::None;
};
uint32_t requestQrScan();
bool requestQrJoin(uint32_t id, const core::wifi_qr::Credentials& credentials,
                   uint32_t startedMs);
// Called by the QR service only after camera/decoder shutdown is acknowledged.
// wifi_mgr owns radio restoration, not decoder-worker cleanup.
void releaseQrSession(uint32_t id);
QrStatus qrStatus();

}  // namespace wifi
}  // namespace hal
