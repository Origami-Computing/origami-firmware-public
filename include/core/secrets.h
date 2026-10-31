#pragma once

#include <stdint.h>

// Runtime-mutable configuration backed by NVS (Preferences namespace "secrets").
//
// What's in here:
//   - WiFi STA credentials (ssid + password)
//   - Backend base URL
//   - Backend API key / auth token
//
// Why a layer exists at all (instead of #define'd macros):
//   - Web UI can change them at runtime and have the change survive a reboot.
//   - Compile-time defaults from `include/core/menu_config.h` still work as the
//     first-boot fallback, so the device boots and connects without any
//     provisioning step.
//   - Single, clearly-named NVS namespace ("secrets") makes it possible to wipe
//     or back up provisioning state in one shot from the settings menu.
//
// Threading: init() runs before consumers start. Backend strings are available
// only as owned snapshots; setters cannot mutate a caller's connection inputs.
// Borrowed Wi-Fi getters remain legacy APIs: copying their result does not
// synchronize with a concurrent setter. NVS writes are not transactional.

namespace core {
namespace secrets {

/** Read every field from NVS, falling back to the WIFI_SSID / WIFI_PASS /
 *  BACKEND_URL / AUTH_TOKEN macros from `core/menu_config.h` when a key is
 *  empty or missing. Call before consumers start, including on reinitialization. */
void init();

/** Convenience: clear every field (sets each NVS key to empty). The next
 *  boot will fall back to the compile-time macros. Returns false on NVS error. */
bool factoryReset();

// ---- WiFi STA -------------------------------------------------------------

/** Maximum number of STA credentials we keep in NVS at once. Sized to match
 *  what `WiFiMulti` will iterate at boot — small enough that scanning all of
 *  them every reconnect costs <100 ms of RAM compares, big enough to cover
 *  "home + office + cafe + phone hotspot" the way a phone does. */
constexpr int kMaxWifiNetworks = 4;

/** Number of populated STA credentials (0..kMaxWifiNetworks). */
int wifiNetworkCount();

/** SSID at slot `i`. Returns "" if `i` is out of range or the slot is empty. */
const char* wifiNetworkSSID(int i);

/** Password at slot `i`. Returns "" if `i` is out of range or the slot is empty. */
const char* wifiNetworkPassword(int i);

/** Append (or update if SSID matches an existing slot) a STA network. Returns
 *  false if every slot is full (and the SSID is new) or NVS write fails. The
 *  HAL is NOT touched — call `hal::wifi::addSTACredential()` + `connect()` to
 *  apply to the live radio. */
bool addWifiNetwork(const char* ssid, const char* password);

/** Remove the network at slot `i` and shift the remaining ones down so there
 *  are no gaps. Returns false on NVS error or out-of-range index. */
bool removeWifiNetwork(int i);

/** Move `ssid` to slot 0 (primary) while keeping other networks in stable
 *  relative order. Call after the user picks a network in the portal so the
 *  next `connect()` tries it first. Returns false if `ssid` is not saved. */
bool promoteWifiNetworkToFront(const char* ssid);

/** SSID of the last STA association we persisted (NVS). Empty if unknown. */
const char* lastConnectedWifi();

/** Remember which SSID last reached full association (for next `connect()`).
 *  Pass "" to clear. */
bool setLastConnectedWifi(const char* ssid);

// --- Legacy single-credential accessors (slot 0 wrappers) -------------------

/** Slot-0 SSID, kept for backward compatibility. Empty string if no creds. */
const char* wifiSSID();

/** Slot-0 password, kept for backward compatibility. */
const char* wifiPassword();

/** Equivalent to `addWifiNetwork(ssid, password)` — kept for older callers
 *  (web UI / boot code) that only had a single credential to think about. */
bool setWifi(const char* ssid, const char* password);

// ---- Backend ---------------------------------------------------------------

// Owned connection inputs, copied together under the cache lock. No allocation,
// storage access or network work; valid after later setters run. Separate setters
// remain separate publications. Token/Wi-Fi state is outside this snapshot.
struct BackendConfig { char url[192]; char apiKey[192]; };
BackendConfig backendConfig();

// Immutable inputs for a backend request. Acquired after any active backend
// setter completes. Network I/O holds no configuration lock. Revision also
// changes when a token changes, so old responses cannot overwrite newer auth.
struct BackendRequest { BackendConfig backend; char token[192]; uint32_t revision; };
BackendRequest backendRequest();
// False if superseded or persistence fails. Comparison, persistence and cache
// publication serialize with backend setters; no spinlock is held across NVS.
bool setDeviceTokenFor(const BackendRequest& request, const char* token);
bool hasDeviceToken();

/** Set the active backend URL; persist it only when development mode is enabled.
 * In production mode a reboot restores the compile-time production origin.
 * Production images reject every URL except the canonical production origin
 * before touching credentials or storage.
 * Returns false on policy rejection or NVS error. */
bool setBackendUrl(const char* url);

/** RAM revision incremented after a successful changed API-key setter.
 * Consumers compare for inequality; it wraps and is not persisted. Initial
 * loading/factory reset retain their existing separate lifecycle semantics. */
uint32_t apiKeyRevision();

/** Persist a new API key. Returns false on NVS error. */
bool setApiKey(const char* key);

// ---- Per-device telemetry token --------------------------------------------

// Token bytes are available only through backendRequest(), never a borrowed
// pointer. Use hasDeviceToken() for readiness observations (not authorization).

/** Explicit provisioning write. Empty clears registration; there is no token
 * macro fallback. Network responses must use setDeviceTokenFor instead.
 * Returns false on NVS error. Backend APIs require init before first use. */
bool setDeviceToken(const char* token);

// ---- Dev / Prod mode -------------------------------------------------------

/** True when using the dev (local IP) backend, false for prod. */
bool isDevMode();

/** Toggle dev/prod mode. Production images reject dev=true, including direct
 * service/tooling calls. Persists to NVS and updates the selected backend URL. */
bool setDevMode(bool dev);

}  // namespace secrets
}  // namespace core
