#include "core/telemetry.h"
#include "hal/hal_wifi.h"

#include <Arduino.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>
#include <atomic>

#include "core/menu_config.h"
#include "core/secrets.h"
#include "core/wifi_join_policy.h"
#include "core/store.h"
#include "hal/hal_ble.h"
#include "hal/hal_tasks.h"

// WiFi implementation with state tracking for stop/restore

namespace hal {
namespace wifi {

static bool s_initialized = false;
static wifi_mode_t s_mode = WIFI_MODE_NULL;
static wifi_mode_t s_savedMode = WIFI_MODE_AP;  // Remember mode for restore
static bool s_wasConnected = false;              // Remember if STA was connected

static volatile RadioState s_radioState = RadioState::StartingWifi;

static const char* radioStateName(RadioState state) {
  switch (state) {
    case RadioState::StartingWifi:  return "starting-wifi";
    case RadioState::WifiOwned:     return "wifi-owned";
    case RadioState::Suspended:     return "suspended";
    case RadioState::RestoringWifi: return "restoring-wifi";
    case RadioState::OfflineBle:    return "offline-ble";
  }
  return "unknown";
}

static void setRadioState(RadioState next, const char* reason) {
  const RadioState previous = s_radioState;
  if (previous == next) return;
  s_radioState = next;
  LOGD("wifi-state", "%s -> %s (%s)\n",
                radioStateName(previous), radioStateName(next),
                reason ? reason : "unspecified");
}

RadioState radioState() { return s_radioState; }
const char* radioStateName() { return radioStateName(s_radioState); }

// STA join watchdog: endless reconnect attempts can starve the rest of the system when AP is absent.
static uint32_t s_staJoinWatchMs   = 0;
static volatile bool s_staJoinBackedOff = false;
static uint32_t s_staNextProbeMs   = 0;

// Radio preference. The user-facing Settings toggle is disabled for now; keep
// this false so a stale NVS value cannot hold WiFi down at boot.
static volatile bool s_preferBle    = false;
// Tracks whether the BLE-mode WiFi teardown has been applied (edge-trigger so we
// only disconnect/restart the radio on the actual transition, not every tick).
static bool          s_bleModeApplied = false;
// True while WiFi is DELIBERATELY suspended by another subsystem (camera/photos
// call stop()/powerDown() to free PSRAM for capture). In that window the BLE/WiFi
// reconcile must stand down entirely — bringing BLE up here would grab the very
// internal RAM the camera needs and crash it. Cleared by restore().
static volatile bool s_wifiSuspended  = false;

/** First STA join attempt window before we disconnect and turn off autoReconnect (ms). */
#ifndef HAL_WIFI_STA_JOIN_GIVEUP_MS
#define HAL_WIFI_STA_JOIN_GIVEUP_MS 20000u
#endif
/** After backoff, optional slow probe with WiFi.begin() — keep large to avoid join spam. */
#ifndef HAL_WIFI_STA_PROBE_INTERVAL_MS
#define HAL_WIFI_STA_PROBE_INTERVAL_MS 90000u
#endif

// Pending-restore flag set by scheduleRestore(); consumed by servicePendingRestore() at top of loop().
static volatile bool s_restorePending = false;
/** When true, defer restore() so APSTA provisioning is not reset by the main loop. */
static bool s_deferWifiRestore = false;

// Pending-connect request set by requestConnect() (any core); consumed by
// servicePendingConnect() on the wifi_mgr task (Core 0). Provisioning callers on
// the UI core must NOT call WiFi.mode()/begin() directly — those fail off Core 0
// after the WiFi-off-UI-core refactor.
static volatile bool s_connectPending     = false;
static wifi_mode_t   s_pendingConnectMode = WIFI_MODE_STA;
static volatile bool s_credentialsChangedPending = false;
static std::atomic<bool> s_reconnectPending{false};

enum class QrCommand : uint8_t { None, Scan, Join, Release };
static portMUX_TYPE s_qrMux = portMUX_INITIALIZER_UNLOCKED;
static QrStatus s_qrStatus;
static QrCommand s_qrCommand = QrCommand::None;
static core::wifi_qr::Credentials s_qrPendingCredentials{};
static uint32_t s_qrRequestedMs = 0;
static uint8_t s_qrDisconnectReason = 0;
// The manager owns these fields; connecting() also reads the active flag.
static volatile bool s_qrActive = false;
static uint32_t s_qrId = 0;
static uint32_t s_qrJoinMs = 0;
static bool s_qrJoining = false;
static core::wifi_qr::Credentials s_qrCredentials{};

QrStatus qrStatus() {
  portENTER_CRITICAL(&s_qrMux);
  const QrStatus result = s_qrStatus;
  portEXIT_CRITICAL(&s_qrMux);
  return result;
}

uint32_t requestQrScan() {
  portENTER_CRITICAL(&s_qrMux);
  if (++s_qrStatus.id == 0) ++s_qrStatus.id;
  s_qrStatus.phase = QrPhase::Preparing;
  s_qrStatus.error = QrError::None;
  s_qrCommand = QrCommand::Scan;
  s_qrDisconnectReason = 0;
  memset(&s_qrPendingCredentials, 0, sizeof(s_qrPendingCredentials));
  const uint32_t id = s_qrStatus.id;
  portEXIT_CRITICAL(&s_qrMux);
  return id;
}

bool requestQrJoin(uint32_t id, const core::wifi_qr::Credentials& credentials,
                   uint32_t startedMs) {
  portENTER_CRITICAL(&s_qrMux);
  const bool accepted = id == s_qrStatus.id && s_qrStatus.phase == QrPhase::Scanning &&
                        s_qrCommand == QrCommand::None;
  if (accepted) {
    s_qrPendingCredentials = credentials;
    s_qrRequestedMs = startedMs;
    s_qrStatus.phase = QrPhase::Connecting;
    s_qrCommand = QrCommand::Join;
    s_qrDisconnectReason = 0;
  }
  portEXIT_CRITICAL(&s_qrMux);
  return accepted;
}

void releaseQrSession(uint32_t id) {
  portENTER_CRITICAL(&s_qrMux);
  if (id == s_qrStatus.id) s_qrCommand = QrCommand::Release;
  portEXIT_CRITICAL(&s_qrMux);
}

static bool qrCurrent() {
  portENTER_CRITICAL(&s_qrMux);
  const bool current = s_qrId == s_qrStatus.id && s_qrCommand == QrCommand::None;
  portEXIT_CRITICAL(&s_qrMux);
  return current;
}

static void publishQr(QrPhase phase, QrError error = QrError::None) {
  portENTER_CRITICAL(&s_qrMux);
  if (s_qrId == s_qrStatus.id && s_qrCommand == QrCommand::None) {
    s_qrStatus.phase = phase;
    s_qrStatus.error = error;
  }
  portEXIT_CRITICAL(&s_qrMux);
  LOGD("wifi-join", "id=%lu phase=%u error=%u elapsed=%lu\n",
                (unsigned long)s_qrId, (unsigned)phase, (unsigned)error,
                (unsigned long)(s_qrJoining ? millis() - s_qrJoinMs : 0));
}

// STA credentials live in core::secrets (NVS-backed). The HAL only mirrors
// them in RAM for the radio API. main_menu.cpp::setup() calls
// core::secrets::init() then pushes every saved network here via
// addSTACredential(); the web /connect handler calls
// core::secrets::addWifiNetwork() (persists) and then addSTACredential()
// (applies to the live radio).
//
// We do NOT use WiFiMulti at runtime: WiFiMulti::run() does a synchronous
// WiFi.scanNetworks() (~2 s) before every begin(), which made app→Kami exits
// noticeably laggy. Instead, `connect()` is non-blocking and just
// `WiFi.begin()`s the first credential; `tickJoinBackoff()` round-robins
// through the rest after each per-AP timeout. This matches phone behavior
// (try every saved AP, stick with whichever joins) without the scan stall.
static const char* DEFAULT_AP_PASS = "origami123";
static char s_apSSID[32] = {0};

static int  s_credCount = 0;
static char s_credSSIDs[kMaxStaCredentials][33] = {{0}};
static char s_credPasses[kMaxStaCredentials][65] = {{0}};


// Index of the credential we're currently trying (0..s_credCount-1). Used by
// tickJoinBackoff to advance round-robin on per-AP timeout.
static int  s_currentTryIdx = 0;

// Number of credentials we've already tried this round-robin cycle. Reset to 1
// (we count the initial connect() attempt as the first try) every time a fresh
// connect() kicks off. Once it reaches s_credCount, we've tried them all and
// drop into the slow-probe backoff phase.
static int  s_triedThisCycle = 0;
/** Bit i set iff slot i was already attempted in the current join cycle. */
static uint32_t s_triedCredMask = 0;

// Mirror of the *primary* (slot-0 / first added) credential, kept around so
// `getSTASSID()` and the legacy single-credential code paths still work even
// when the cred list is empty.
static char s_staSSID[33] = {0};
static char s_staPass[65] = {0};

void applyModemSleepPolicy() {
  if (!s_initialized) return;
  // ESP-IDF + coexistence: BLE stack active ⇒ modem sleep must stay enabled or Wi-Fi tasks abort in
  // `wifi_set_ps_process` / `pm_set_sleep_type` ("Should enable WiFi modem sleep when both WiFi and BT").
  // Without BLE (or before NimBLE starts), keep modem sleep off — softer DMA/I²S timing historically.
  if (hal::ble::started()) {
    WiFi.setSleep(true);
  } else {
    WiFi.setSleep(false);
  }
}

bool init() {
  if (s_initialized) return true;

  // Must run before WiFi.mode()/esp_wifi_init(). On the pinned S3 SDK, static
  // mode reserves eight RX and eight TX buffers even while idle. Arduino's
  // dynamic mode retains four RX buffers and allocates TX on demand. Our
  // esp_wifi_init boundary caps its TX pool at eight to preserve internal/DMA
  // memory for TLS AES and audio during sustained uploads as well as idle.
  WiFi.useStaticBuffers(false);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    portENTER_CRITICAL(&s_qrMux);
    if (s_qrStatus.phase == QrPhase::Connecting && s_qrCommand == QrCommand::None)
      s_qrDisconnectReason = info.wifi_sta_disconnected.reason;
    portEXIT_CRITICAL(&s_qrMux);
  }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  Serial.println("[wifi] Initializing...");
  
  // Generate AP SSID from MAC
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
  snprintf(s_apSSID, sizeof(s_apSSID), "Origami_%02X%02X%02X", mac[3], mac[4], mac[5]);
  
  Serial.printf("[wifi] AP SSID: %s\n", s_apSSID);

  // Force WiFi preference. Clear old builds' persisted BLE-mode value so boot
  // cannot intentionally keep WiFi down.
  if (core::store::getBool("settings", "ble_mode", false)) {
    core::store::putBool("settings", "ble_mode", false);
    Serial.println("[wifi] cleared stale BLE radio preference");
  }
  s_preferBle = false;
  Serial.println("[wifi] radio preference: WiFi");

  s_initialized = true;
  return true;
}

bool setMode(wifi_mode_t mode) {
  if (!s_initialized) return false;

  if (mode != WIFI_OFF) {
    setRadioState(RadioState::RestoringWifi, "WiFi mode change");
    if (hal::ble::started()) {
      hal::ble::deinit();
      vTaskDelay(pdMS_TO_TICKS(80));
    }
  }

  Serial.printf("[wifi] Setting mode %d...\n", mode);

  bool ok = WiFi.mode((wifi_mode_t)mode);
  Serial.printf("[wifi] Mode result: %s\n", ok ? "OK" : "FAIL");
  if (ok) {
    s_mode      = mode;
    s_savedMode = mode;
  }
  return ok;
}

wifi_mode_t getMode() {
  return s_mode;
}

bool addSTACredential(const char* ssid, const char* password) {
  if (!ssid || !ssid[0]) return false;
  // Update-in-place if we already know this SSID (lets `/connect` fix a typo'd
  // password without bloating the list).
  for (int i = 0; i < s_credCount; i++) {
    if (strcmp(s_credSSIDs[i], ssid) == 0) {
      strncpy(s_credPasses[i], password ? password : "", sizeof(s_credPasses[0]) - 1);
      s_credPasses[i][sizeof(s_credPasses[0]) - 1] = '\0';
      Serial.printf("[wifi] STA cred updated in-place: '%s' (slot %d)\n", ssid, i);
      // Slot 0 is the legacy "primary" — keep the mirror in sync.
      if (i == 0) {
        strncpy(s_staSSID, ssid, sizeof(s_staSSID) - 1); s_staSSID[sizeof(s_staSSID) - 1] = '\0';
        strncpy(s_staPass, password ? password : "", sizeof(s_staPass) - 1); s_staPass[sizeof(s_staPass) - 1] = '\0';
      }
      return true;
    }
  }
  if (s_credCount >= kMaxStaCredentials) {
    Serial.printf("[wifi] STA cred list full (%d/%d), rejecting '%s'\n",
                  s_credCount, kMaxStaCredentials, ssid);
    return false;
  }
  int slot = s_credCount;
  strncpy(s_credSSIDs[slot],  ssid, sizeof(s_credSSIDs[0])  - 1);
  s_credSSIDs[slot][sizeof(s_credSSIDs[0]) - 1] = '\0';
  strncpy(s_credPasses[slot], password ? password : "", sizeof(s_credPasses[0]) - 1);
  s_credPasses[slot][sizeof(s_credPasses[0]) - 1] = '\0';
  s_credCount++;
  if (slot == 0) {
    strncpy(s_staSSID, ssid, sizeof(s_staSSID) - 1); s_staSSID[sizeof(s_staSSID) - 1] = '\0';
    strncpy(s_staPass, password ? password : "", sizeof(s_staPass) - 1); s_staPass[sizeof(s_staPass) - 1] = '\0';
  }
  Serial.printf("[wifi] STA cred added: slot=%d ssid='%s' total=%d\n", slot, ssid, s_credCount);
  return true;
}

void clearSTACredentials() {
  s_credCount = 0;
  s_currentTryIdx = 0;
  s_triedCredMask = 0;
  for (int i = 0; i < kMaxStaCredentials; i++) {
    s_credSSIDs[i][0]  = '\0';
    s_credPasses[i][0] = '\0';
  }
  s_staSSID[0] = '\0';
  s_staPass[0] = '\0';
  Serial.println("[wifi] STA cred list cleared");
}

int staCredentialCount() { return s_credCount; }

void reloadCredentialsFromSecrets() {
  clearSTACredentials();
  const int n = core::secrets::wifiNetworkCount();
  for (int i = 0; i < n; i++) {
    addSTACredential(core::secrets::wifiNetworkSSID(i), core::secrets::wifiNetworkPassword(i));
  }
  Serial.printf("[wifi] reloadCredentialsFromSecrets: %d network(s)\n", s_credCount);
}

bool setSTACredentials(const char* ssid, const char* password) {
  // Backward-compat shim: callers expecting a single-credential setter still
  // work — we just blow away the list and install the one they asked for.
  clearSTACredentials();
  return addSTACredential(ssid, password);
}

bool setAPCredentials(const char* ssid, const char* password, uint8_t channel, uint8_t maxConnections) {
  // Not implemented - using defaults
  return true;
}

// Tracks WL_CONNECTED edge for persisting "last good" SSID into NVS.
static bool s_wasStaConnected = false;

// Pick which credential index to try first when (re)starting a join cycle.
// Order: last successful association (NVS) → live BSSID match if already up → slot 0.
static int pickStartIdx() {
  const char* pref = core::secrets::lastConnectedWifi();
  if (pref && pref[0]) {
    for (int i = 0; i < s_credCount; i++) {
      if (strcmp(s_credSSIDs[i], pref) == 0) return i;
    }
  }
  if (connected()) {
    String live = WiFi.SSID();
    for (int i = 0; i < s_credCount; i++) {
      if (live.equals(s_credSSIDs[i])) return i;
    }
  }
  return 0;
}

/** Next untried slot after `s_currentTryIdx` (circular). No RF scan. */
static int nextUntriedSlotCircular() {
  for (int k = 1; k < s_credCount; k++) {
    int i = (s_currentTryIdx + k) % s_credCount;
    if (!(s_triedCredMask & (1u << i))) return i;
  }
  return -1;
}

// Manager-owned operation. UI callers only publish reconnect intent below.
static bool connectNow() {
  if (!s_initialized) return false;
  if (s_credCount == 0) {
    Serial.println("[wifi] connect: no STA credentials registered");
    return false;
  }
  s_staJoinBackedOff = false;
  // Manual multi-network rotation fights Arduino auto-reconnect — disable when
  // we own the join sequence.
  WiFi.setAutoReconnect(s_credCount <= 1);

  if (!connected()) {
    WiFi.disconnect(false);
  }

  // Non-blocking slot selection: use NVS last-connected SSID preference.
  // (pickJoinSlotByScan was removed — WiFi.scanNetworks() blocks 2-5 s on the
  // main loop and freezes the UI. The WiFi driver will pick the best AP at
  // association time without a host-side scan.)
  const int first = pickStartIdx();
  s_currentTryIdx  = first;
  s_triedThisCycle   = 1;
  s_triedCredMask    = (1u << first);
  s_staJoinWatchMs   = millis();

  Serial.printf("[wifi] connect: slot %d/%d ssid='%s' wl=%d\n", s_currentTryIdx, s_credCount,
                s_credSSIDs[s_currentTryIdx], (int)WiFi.status());
  WiFi.begin(s_credSSIDs[s_currentTryIdx], s_credPasses[s_currentTryIdx]);
  return true;
}

bool connect() {
  if (!s_initialized || s_credCount == 0) return false;
  s_reconnectPending.store(true, std::memory_order_release);
  return true;
}

static void servicePendingReconnect() {
  if (!s_reconnectPending.exchange(false, std::memory_order_acq_rel)) return;
  // Home/voice entry must not interrupt an in-flight authentication, restart
  // an intentional camera/QR suspension, or supersede provisioning intent.
  if (s_wifiSuspended || s_deferWifiRestore || s_preferBle || s_qrActive ||
      s_connectPending || s_restorePending || connected() || connecting()) return;
  connectNow();
}

// BLE/WiFi RAM coexistence rule. BLE + WiFi up together is the device's biggest
// internal-DRAM drain — it starves hardware AES (esp-aes "Failed to allocate
// memory") and breaks all TLS (agent WS, OTA, telemetry). Rule: BLE is up ONLY
// while WiFi is DOWN (provisioning / offline). Reconciled on the wifi-task tick;
// guarded so it's idempotent and a no-op when already in the right state.
void reconcileBleWithWifi() {
  // Credential count says what we can join; it does not say whether the shared
  // radio is safe to reconfigure. BLE must stay down throughout camera ownership
  // and WiFi startup, even while WiFi.status() reports disconnected.
  const RadioState state = s_radioState;
  if (state == RadioState::StartingWifi ||
      state == RadioState::Suspended ||
      state == RadioState::RestoringWifi) {
    return;
  }

  // User chose BLE (Settings → Radio): take WiFi down, keep BLE up. The radio
  // teardown is edge-triggered so we don't thrash it every tick.
  if (s_preferBle) {
    if (!s_bleModeApplied) {
      WiFi.setAutoReconnect(false);
      WiFi.disconnect(true);   // drop STA + power off radio → frees coexistence RAM
      s_bleModeApplied = true;
      Serial.println("[wifi] radio=BLE: WiFi down");
    }
    if (!hal::ble::started() && hal::ble::init(BLE_DEVICE_NAME)) {
      applyModemSleepPolicy();
      Serial.println("[wifi] radio=BLE: BLE up");
    }
    return;
  }

  // Leaving BLE mode → bring WiFi back online (reconnect); BLE is dropped below
  // once we associate.
  if (s_bleModeApplied) {
    s_bleModeApplied = false;
    setMode(WIFI_MODE_STA);
    s_staJoinBackedOff = false;
    connectNow();
    Serial.println("[wifi] radio=WiFi: rejoining");
  }

  if (state == RadioState::OfflineBle) {
    if (!hal::ble::started() && hal::ble::init(BLE_DEVICE_NAME)) {
      applyModemSleepPolicy();
      Serial.println("[wifi] BLE up (radio confirmed offline)");
    }
    return;
  }

  // Normal WiFi-priority rule: BLE is down whenever WiFi owns the radio,
  // including transient association failures.
  const bool wifiUp = connected();
  if (wifiUp && hal::ble::started()) {
    hal::ble::deinit();
    applyModemSleepPolicy();
    Serial.println("[wifi] BLE down (WiFi up — reclaim internal RAM for TLS)");
  }
}

void tickJoinBackoff() {
  if (!s_initialized) return;
  reconcileBleWithWifi();
  if (s_preferBle) return;   // user chose BLE — don't attempt WiFi joins
  const uint32_t now = millis();

  if (connected()) {
    setRadioState(RadioState::WifiOwned, "STA connected");
    if (!s_wasStaConnected) {
      s_wasStaConnected = true;
      String s = WiFi.SSID();
      if (s.length() > 0) {
        // Persist as last-connected AND promote it to slot 0 so the next boot
        // tries this SSID first regardless of scan results (and the "primary"
        // log line stops lying).
        core::secrets::setLastConnectedWifi(s.c_str());
        if (core::secrets::promoteWifiNetworkToFront(s.c_str())) {
          reloadCredentialsFromSecrets();
        }
        Serial.printf("[wifi] joined '%s' -> promoted to slot 0\n", s.c_str());
      }
    }
    s_staJoinBackedOff = false;
    return;
  }
  s_wasStaConnected = false;
  if (s_credCount == 0) return;
  const wifi_mode_t m = WiFi.getMode();
  if (m == WIFI_OFF || !(m & WIFI_MODE_STA)) return;
  // Do NOT bail out just because AP is also up (APSTA). That used to skip this
  // entire function whenever the soft-AP bit was set — so after `/connect` put
  // the radio in APSTA, join retries / post-backoff probes never ran and STA
  // could sit forever in IDLE/CONNECTING. `WiFi.disconnect()` here only affects
  // the STA interface; pure `WIFI_MODE_AP` still exits above (no STA bit).

  if (!s_staJoinBackedOff) {
    if ((int32_t)(now - s_staJoinWatchMs) < (int32_t)HAL_WIFI_STA_JOIN_GIVEUP_MS) return;

    // This per-AP attempt timed out. Round-robin to the next saved network
    // before falling back to the slow probe — that's what gives "phone-style"
    // try-them-all behavior without a single big upfront scan.
    if (s_credCount > 1 && s_triedThisCycle < s_credCount) {
      const int next = nextUntriedSlotCircular();
      if (next < 0) {
        Serial.println("[wifi] round-robin: internal mask error, forcing backoff");
      } else {
        s_currentTryIdx   = next;
        s_triedCredMask  |= (1u << next);
        s_triedThisCycle++;
        s_staJoinWatchMs = now;
        WiFi.disconnect(false);
        Serial.printf("[wifi] STA try (%d/%d): slot %d ssid='%s' wl=%d\n", s_triedThisCycle,
                      s_credCount, s_currentTryIdx, s_credSSIDs[s_currentTryIdx], (int)WiFi.status());
        WiFi.begin(s_credSSIDs[s_currentTryIdx], s_credPasses[s_currentTryIdx]);
        return;
      }
    }

    LOGD("wifi", "STA join failed all %d net(s), %u ms each, last wl=%d — backoff\n",
                  s_credCount, (unsigned)HAL_WIFI_STA_JOIN_GIVEUP_MS, (int)WiFi.status());
    WiFi.disconnect(false);
    WiFi.setAutoReconnect(false);
    s_staJoinBackedOff = true;
    s_staNextProbeMs   = now + HAL_WIFI_STA_PROBE_INTERVAL_MS;
    return;
  }

  if ((int32_t)(now - s_staNextProbeMs) >= 0) {
    WiFi.setAutoReconnect(s_credCount <= 1);
    WiFi.disconnect(false);
    const int idx = pickStartIdx();
    s_currentTryIdx    = idx;
    s_triedThisCycle     = 1;
    s_triedCredMask      = (1u << idx);
    s_staJoinBackedOff   = false;
    s_staJoinWatchMs     = millis();
    Serial.printf("[wifi] STA probe (post-backoff): slot %d ssid='%s' wl=%d\n", s_currentTryIdx,
                  s_credSSIDs[s_currentTryIdx], (int)WiFi.status());
    WiFi.begin(s_credSSIDs[s_currentTryIdx], s_credPasses[s_currentTryIdx]);
  }
}

void disconnect() {
  WiFi.disconnect();
}

void setPreferBle(bool on) {
  if (on) {
    Serial.println("[wifi] setPreferBle ignored: BLE radio preference disabled");
  }
  s_preferBle = false;
  core::store::putBool("settings", "ble_mode", false);
}

bool preferBle() { return s_preferBle; }

bool connected() {
  return WiFi.status() == WL_CONNECTED;
}

bool connecting() {
  if (connected()) return false;
  const auto qr = qrStatus();
  if (s_qrActive && qr.phase == QrPhase::Connecting) return true;
  if (s_wifiSuspended || s_preferBle) return false;
  return s_initialized && s_credCount > 0 && !s_staJoinBackedOff &&
         (WiFi.getMode() & WIFI_MODE_STA);
}

bool suspended() {
  return s_wifiSuspended;
}

esp_ip4_addr_t getSTAIP() {
  esp_ip4_addr_t ip = {0};
  if (connected()) ip.addr = (uint32_t)WiFi.localIP();
  return ip;
}

bool startAP() {
  if (!s_initialized) return false;
  
  Serial.printf("[wifi] Starting AP '%s'...\n", s_apSSID);
  
  bool ok = WiFi.softAP(s_apSSID, DEFAULT_AP_PASS, 1, 0, 4);
  
  if (ok) {
    applyModemSleepPolicy();
    s_wifiSuspended = false;
    setRadioState(RadioState::WifiOwned, "soft AP started");
    Serial.println(hal::ble::started() ? "[wifi] Modem sleep ON (BLE coexistence)"
                                     : "[wifi] Modem sleep OFF (no BLE)");
    
    Serial.printf("[wifi] AP IP: %s\n", WiFi.softAPIP().toString().c_str());
  } else {
    Serial.println("[wifi] AP FAILED");
  }
  return ok;
}

void stopAP() {
  WiFi.softAPdisconnect(true);
}

bool apStarted() {
  return WiFi.getMode() & WIFI_MODE_AP;
}

esp_ip4_addr_t getAPIP() {
  esp_ip4_addr_t ip = {0};
  ip.addr = (uint32_t)WiFi.softAPIP();
  return ip;
}

uint8_t getAPClientCount() {
  return WiFi.softAPgetStationNum();
}

bool scan(wifi_ap_record_t* results, uint16_t* count, uint16_t maxResults) {
  if (!results || !count) return false;
  
  int found = WiFi.scanNetworks(false, true);
  if (found < 0) { *count = 0; return false; }
  
  *count = min((uint16_t)found, maxResults);
  for (int i = 0; i < *count; i++) {
    memset(&results[i], 0, sizeof(wifi_ap_record_t));
    strncpy((char*)results[i].ssid, WiFi.SSID(i).c_str(), 32);
    results[i].rssi = WiFi.RSSI(i);
  }
  WiFi.scanDelete();
  return true;
}

void stop() {
  // Save current state before stopping
  s_wasConnected = connected();
  s_restorePending = false;
  s_wifiSuspended = true;  // freeze the BLE/WiFi reconcile until restore()
  setRadioState(RadioState::Suspended, "temporary stop");

  Serial.println("[wifi] Stopping...");
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  // No delay here: the old delay(100) blocked the UI thread. yield() lets TCP/IP flush w/o blocking.
  yield();
  Serial.println("[wifi] Stopped");
}

void powerDown() {
  // Step 1: clean STA / AP state and stop the driver (same as stop()).
  s_wasConnected   = connected();
  s_restorePending = false;
  s_wifiSuspended = true;  // freeze the BLE/WiFi reconcile until restore()
  setRadioState(RadioState::Suspended, "deep power-down");
  Serial.println("[wifi] Powering down (deep deinit)...");
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  yield();

  // Keep the explicit deinit attempt and settling delay used by high-res
  // capture. The pinned Arduino 2.0.17 espWiFiStop() already calls deinit via
  // WIFI_OFF, so this may simply report NOT_INIT. Do not infer extra PSRAM
  // recovery over stop() from this second call; measure it before simplifying.
  esp_err_t err = esp_wifi_deinit();
  if (err == ESP_OK) {
    Serial.println("[wifi] esp_wifi_deinit OK — driver pools freed");
  } else if (err == ESP_ERR_WIFI_NOT_INIT) {
    Serial.println("[wifi] esp_wifi_deinit: driver was not init — nothing to free");
  } else {
    Serial.printf("[wifi] esp_wifi_deinit failed: 0x%04x (continuing)\n", (unsigned)err);
  }
  // Give the WiFi task a chance to fully tear down before any caller measures heap.
  vTaskDelay(pdMS_TO_TICKS(80));
  Serial.println("[wifi] Powered down");
}

bool restore() {
  Serial.printf("[wifi %lu] restore(): begin savedMode=%d wasConnected=%d\n",
                (unsigned long)millis(), (int)s_savedMode, s_wasConnected ? 1 : 0);
  s_restorePending = false;

  // With no STA credentials and no AP to restore, WiFi is already completely
  // stopped by stop()/powerDown(). Do not restart STA only to let the next tick
  // race BLE against esp_wifi_init; hand the settled radio directly to BLE.
  if (s_credCount == 0 && s_savedMode == WIFI_MODE_STA) {
    s_wifiSuspended = false;
    setRadioState(RadioState::OfflineBle, "restore skipped: no credentials");
    Serial.println("[wifi] Restore skipped: no STA credentials; WiFi remains off");
    return true;
  }

  setRadioState(RadioState::RestoringWifi, "restore requested");

  if (!WiFi.mode(s_savedMode)) {
    Serial.println("[wifi] Mode restore failed");
    setRadioState(RadioState::Suspended, "mode restore failed");
    return false;
  }
  yield();

  if (s_savedMode == WIFI_MODE_AP || s_savedMode == WIFI_MODE_APSTA) {
    if (!WiFi.softAP(s_apSSID, DEFAULT_AP_PASS, 1, 0, 4)) {
      Serial.println("[wifi] AP restore failed");
      setRadioState(RadioState::Suspended, "AP restore failed");
      return false;
    }
    Serial.printf("[wifi] AP restored: %s\n", WiFi.softAPIP().toString().c_str());
  }

  // Always (re)issue an STA join when the saved mode includes STA and we have credentials. The
  // previous `s_wasConnected` gate meant that if the user entered camera while WiFi was still
  // mid-join, we would silently drop STA on exit and the chrome would say "wifi=not connected"
  // forever. Issuing the join unconditionally is the only way to guarantee an attempt is in
  // flight after restore. We funnel through connect() so the multi-credential path is honored.
  if ((s_savedMode == WIFI_MODE_STA || s_savedMode == WIFI_MODE_APSTA) && s_credCount > 0) {
    Serial.printf("[wifi] Reconnecting (%d nets, was_connected=%d)...\n", s_credCount,
                  s_wasConnected ? 1 : 0);
    connectNow();
  }

  applyModemSleepPolicy();
  s_wifiSuspended = false;

  if (s_savedMode == WIFI_MODE_AP) {
    setRadioState(RadioState::WifiOwned, "AP restored");
  }

  yield();
  Serial.println("[wifi] Restored");
  return true;
}

void scheduleRestore() {
  Serial.printf("[wifi %lu] scheduleRestore() (pending restore on next servicePendingRestore)\n",
                (unsigned long)millis());
  s_restorePending = true;
}

void requestConnect(wifi_mode_t mode) {
  // Safe to call from any core: just records the request. The blocking radio
  // calls run in servicePendingConnect() on the wifi_mgr task (Core 0).
  s_pendingConnectMode = mode;
  s_connectPending     = true;
}

void servicePendingConnect() {
  if (!s_connectPending) return;
  if (!s_initialized) { s_connectPending = false; return; }
  s_connectPending = false;
  // A successful QR parse can enqueue connect before exitApp enqueues restore.
  // One manager-owned connect supersedes that stale restore request.
  s_restorePending = false;

  reloadCredentialsFromSecrets();
  if (s_credCount == 0) {
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    vTaskDelay(pdMS_TO_TICKS(80));
    s_wifiSuspended = false;
    setRadioState(RadioState::OfflineBle, "connect requested without credentials");
    return;
  }

  s_wifiSuspended = true;
  setRadioState(RadioState::RestoringWifi, "connect requested");
  if (hal::ble::started()) {
    hal::ble::deinit();
    vTaskDelay(pdMS_TO_TICKS(80));
  }

  const wifi_mode_t mode = s_pendingConnectMode;
  if (mode == WIFI_MODE_STA) stopAP();
  if (!setMode(mode)) {
    Serial.println("[wifi] servicePendingConnect: setMode failed");
    setRadioState(RadioState::Suspended, "connect mode failed");
    return;
  }
  applyModemSleepPolicy();
  s_wifiSuspended = false;
  connectNow();
}

void requestCredentialsChanged() {
  s_credentialsChangedPending = true;
}

void servicePendingCredentialsChanged() {
  if (!s_credentialsChangedPending) return;
  if (!s_initialized) {
    s_credentialsChangedPending = false;
    return;
  }
  s_credentialsChangedPending = false;

  // If camera/photos already own the radio, only refresh the manager's cache.
  // The later restore request will choose WiFi or BLE from the new count.
  if (s_wifiSuspended) {
    reloadCredentialsFromSecrets();
    Serial.println("[wifi] credentials refreshed while radio suspended");
    return;
  }

  s_wifiSuspended = true;
  setRadioState(RadioState::Suspended, "credentials changed");
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  vTaskDelay(pdMS_TO_TICKS(80));

  reloadCredentialsFromSecrets();
  if (s_credCount == 0) {
    s_wifiSuspended = false;
    setRadioState(RadioState::OfflineBle, "no credentials remain");
    return;
  }

  setRadioState(RadioState::RestoringWifi, "credentials remain");
  if (!setMode(WIFI_MODE_STA)) {
    setRadioState(RadioState::Suspended, "credential reconnect mode failed");
    return;
  }
  s_wifiSuspended = false;
  connectNow();
}

void setWifiRestoreDeferred(bool defer) { s_deferWifiRestore = defer; }

void servicePendingRestore() {
  if (!s_restorePending) return;
  if (s_deferWifiRestore) return;
  if (!s_initialized) {
    s_restorePending = false;
    return;
  }
  // Always run restore(). Previously this checked `WiFi.getMode() != WIFI_OFF` and bailed early,
  // but that left the device wedged after camera exit: driver was already in STA mode from before
  // stop() (mode setting is async / can race), so begin() never re-fired and the join machine
  // sat dormant. restore() is idempotent for mode and re-issues WiFi.begin().
  Serial.printf("[wifi %lu] servicePendingRestore: calling restore() (mode=%d status=%d)\n",
                (unsigned long)millis(), (int)WiFi.getMode(), (int)WiFi.status());
  s_restorePending = false;
  (void)restore();
}

void deinit() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  s_initialized = false;
}

const char* getSTASSID() {
  // Prefer the live SSID when connected — in a multi-network setup the slot-0
  // mirror may be stale (e.g. boot scanned and picked slot 2). Fall back to
  // the slot-0 mirror so callers always see something stable when we're not
  // joined yet.
  if (connected()) {
    static char s_liveSSID[33] = {0};
    String s = WiFi.SSID();
    strncpy(s_liveSSID, s.c_str(), sizeof(s_liveSSID) - 1);
    s_liveSSID[sizeof(s_liveSSID) - 1] = '\0';
    if (s_liveSSID[0]) return s_liveSSID;
  }
  return s_staSSID;
}
const char* getAPSSID() { return s_apSSID; }

// ---------------------------------------------------------------------------
// WiFi manager task (Option B)
//
// Runs on Core 0 so all blocking WiFi API calls (WiFi.mode, WiFi.begin,
// WiFi.disconnect) happen off the UI core. The main loop (Core 1) only reads
// status via connected() / apStarted() — both thread-safe in ESP-IDF.
//
// Boot sequence: replaces the first-iteration block that was in services::tick().
// Maintenance loop: tickJoinBackoff + servicePendingRestore every 500 ms.
//   tickJoinBackoff early-returns when mode == WIFI_OFF (camera / powerDown)
//   or when STA bit is not set (portal AP-only mode), so it never fights apps
//   that have taken direct control of the radio.
// ---------------------------------------------------------------------------

static volatile bool s_taskBooted = false;

static bool stopQrRadio() {
  s_wifiSuspended = true;
  setRadioState(RadioState::Suspended, "QR session");
  if (hal::ble::started()) hal::ble::deinit();
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true);
  const bool stopped = WiFi.mode(WIFI_OFF);
  vTaskDelay(pdMS_TO_TICKS(80));
  s_wasStaConnected = false;
  if (!stopped) Serial.println("[wifi-join] radio stop failed; scanner not admitted");
  return stopped;
}

static void failQr(QrError error) {
  // Keep the radio off until retry/exit: no background fallback or late success.
  stopQrRadio();
  publishQr(QrPhase::Failed, error);
  s_qrJoining = false;
  memset(&s_qrCredentials, 0, sizeof(s_qrCredentials));
}

static bool serviceQrSession() {
  core::wifi_qr::Credentials credentials{};
  portENTER_CRITICAL(&s_qrMux);
  const QrCommand command = s_qrCommand;
  const uint32_t id = s_qrStatus.id;
  const uint32_t requestedMs = s_qrRequestedMs;
  if (command == QrCommand::Join) credentials = s_qrPendingCredentials;
  s_qrCommand = QrCommand::None;
  memset(&s_qrPendingCredentials, 0, sizeof(s_qrPendingCredentials));
  portEXIT_CRITICAL(&s_qrMux);

  if (command == QrCommand::Scan) {
    s_qrActive = true;
    s_qrId = id;
    s_qrJoining = false;
    memset(&s_qrCredentials, 0, sizeof(s_qrCredentials));
    // Explicit setup supersedes pending background restoration/connection.
    s_restorePending = s_connectPending = false;
    const bool stopped = stopQrRadio();
    publishQr(stopped ? QrPhase::Scanning : QrPhase::Failed,
              stopped ? QrError::None : QrError::Radio);
  } else if (command == QrCommand::Release) {
    if (s_qrActive) {
      if (s_qrJoining || !connected()) stopQrRadio();
      s_qrJoining = s_qrActive = false;
      memset(&s_qrCredentials, 0, sizeof(s_qrCredentials));
      reloadCredentialsFromSecrets();
      // Connected sessions (including save failures) remain on the live target.
      // Failed/cancelled sessions restore only previously saved credentials.
      s_wifiSuspended = false;
      s_savedMode = WIFI_MODE_STA;
      if (connected()) {
        s_wasStaConnected = true;
        setRadioState(RadioState::WifiOwned, "QR finished");
      } else {
        scheduleRestore();
      }
    }
  } else if (command == QrCommand::Join && s_qrActive && id == s_qrId) {
    s_qrCredentials = credentials;
    s_qrJoinMs = requestedMs;
    s_qrJoining = true;
    bool existing = false;
    for (int i = 0; i < core::secrets::wifiNetworkCount(); ++i) {
      if (strcmp(credentials.ssid, core::secrets::wifiNetworkSSID(i)) == 0) existing = true;
    }
    if (!existing && core::secrets::wifiNetworkCount() >= core::secrets::kMaxWifiNetworks) {
      failQr(QrError::NetworksFull);
    } else if (!credentials.ssid[0] || !setMode(WIFI_MODE_STA)) {
      failQr(QrError::Radio);
    } else if (qrCurrent()) {
      WiFi.setAutoReconnect(false);
      applyModemSleepPolicy();
      WiFi.begin(credentials.ssid, credentials.pass);
      publishQr(QrPhase::Connecting);
    }
  }

  if (s_qrActive && s_qrJoining && qrCurrent()) {
    portENTER_CRITICAL(&s_qrMux);
    const uint8_t reason = s_qrDisconnectReason;
    portEXIT_CRITICAL(&s_qrMux);
    wifi_ap_record_t ap{};
    const bool target = esp_wifi_sta_get_ap_info(&ap) == ESP_OK &&
                        strcmp((const char*)ap.ssid, s_qrCredentials.ssid) == 0;
    const auto outcome = core::wifi_join::assess(millis(), s_qrJoinMs, s_qrCredentials.ssid,
                                                target ? (const char*)ap.ssid : nullptr,
                                                connected(), getSTAIP().addr);
    if (outcome == core::wifi_join::Result::TimedOut) {
      failQr(target ? QrError::NoIp :
             (reason == WIFI_REASON_AUTH_FAIL ? QrError::Authentication : QrError::Timeout));
    } else if (outcome == core::wifi_join::Result::Joined) {
      // Never replace known-good credentials until this exact target has an IP.
      const bool saved = core::secrets::addWifiNetwork(s_qrCredentials.ssid, s_qrCredentials.pass);
      bool keySaved = true;
      if (saved && s_qrCredentials.hasApiKey) keySaved = core::secrets::setApiKey(s_qrCredentials.apiKey);
      if (saved) (void)core::secrets::setLastConnectedWifi(s_qrCredentials.ssid);
      reloadCredentialsFromSecrets();
      s_wasStaConnected = true;
      setRadioState(RadioState::WifiOwned, "QR target has IP");
      publishQr(saved && keySaved ? QrPhase::Joined : QrPhase::Failed,
                saved && keySaved ? QrError::None : QrError::SaveFailed);
      s_qrJoining = false;
      memset(&s_qrCredentials, 0, sizeof(s_qrCredentials));
    } else if (WiFi.status() == WL_NO_SSID_AVAIL) {
      failQr(QrError::NetworkNotFound);
    } else if (WiFi.status() == WL_CONNECT_FAILED && reason == WIFI_REASON_AUTH_FAIL) {
      failQr(QrError::Authentication);
    }
  }
  return s_qrActive;
}

static void wifiManagerTask(void*) {
  Serial.println("[wifi-task] boot: init + STA connect");
  init();
  const int n = core::secrets::wifiNetworkCount();
  const char* lastOk = core::secrets::lastConnectedWifi();
  reloadCredentialsFromSecrets();
  if (s_credCount == 0) {
    WiFi.mode(WIFI_OFF);
    s_wifiSuspended = false;
    setRadioState(RadioState::OfflineBle, "boot without credentials");
    Serial.println("[wifi-task] no STA credentials; WiFi remains off");
  } else if (setMode(WIFI_MODE_STA)) {
    setRadioState(RadioState::RestoringWifi, "boot join");
    connectNow();
    if (lastOk && lastOk[0])
      Serial.printf("[wifi-task] STA: %d net(s), prefer \"%s\"\n", n, lastOk);
    else
      Serial.printf("[wifi-task] STA: %d net(s)\n", n);
  } else {
    Serial.println("[wifi-task] setMode(STA) failed");
  }

  // Give WiFi driver a moment before BLE init — same 80 ms that was in
  // services::tick(), but now it only blocks this Core-0 task, not the UI.
  vTaskDelay(pdMS_TO_TICKS(80));

  // BLE is brought up only while WiFi is down (see reconcileBleWithWifi). At
  // boot we're not yet associated, so this enables BLE for provisioning; it is
  // torn down automatically once WiFi connects to free internal RAM for TLS.
  reconcileBleWithWifi();

  s_taskBooted = true;
  Serial.println("[wifi-task] boot complete");

  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(100));
    if (serviceQrSession()) continue;
    servicePendingCredentialsChanged();
    servicePendingConnect();
    tickJoinBackoff();
    servicePendingRestore();
    servicePendingReconnect();
  }
}

void startTask() {
  hal::tasks::spawn(hal::tasks::Id::WifiMgr, wifiManagerTask);
}

bool taskBooted() {
  return s_taskBooted;
}

}  // namespace wifi
}  // namespace hal
