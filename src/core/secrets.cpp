#include "core/secrets.h"
#include "core/release_policy.h"

#include <Arduino.h>
#include <Preferences.h>
#include <string.h>

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/semphr.h>

#include "core/menu_config.h"  // WIFI_SSID, WIFI_PASS, BACKEND_URL, AUTH_TOKEN compile-time fallbacks

namespace core {
namespace secrets {

namespace {

constexpr char kNs[] = "secrets";
// Legacy single-pair keys. Kept readable for one-time migration into the
// multi-network slots below (newer firmware writes only the slotted keys).
constexpr char kKeyWifiSsid[] = "wifi_ssid";
constexpr char kKeyWifiPass[] = "wifi_pass";
constexpr char kKeyBackend[]  = "backend_url";
constexpr char kKeyApi[]      = "api_key";
constexpr char kKeyDevToken[] = "dev_token";  // per-device telemetry bearer token
constexpr char kKeyDevMode[]  = "dev_mode";
constexpr char kKeyBackendDev[] = "backend_dev";
constexpr char kKeyWifiLastOk[] = "wifi_last_ok";

// Per-slot WiFi credential keys. Format: `wifi_s_<i>` / `wifi_p_<i>`. NVS keys
// must be ≤15 chars including NUL, so the short prefix matters.
constexpr char kKeyWifiSsidFmt[] = "wifi_s_%d";
constexpr char kKeyWifiPassFmt[] = "wifi_p_%d";

// In-RAM cache. Sized to comfortable upper bounds — these are user-typed
// strings, not arbitrary blobs. SSID per IEEE 802.11 is up to 32 bytes (+NUL).
// WPA passphrase is 8..63 chars. URL/key are policy-driven; 192 fits a typical
// `https://host:port/path` and a UUID/JWT-ish token.
char g_wifiSsid[kMaxWifiNetworks][33] = {{0}};
char g_wifiPass[kMaxWifiNetworks][65] = {{0}};
int  g_wifiCount = 0;
char g_wifiLastOk[33] = {0};
char g_backendUrl[192] = {0};
char g_apiKey[192]     = {0};
std::atomic<uint32_t> g_apiKeyRevision{0};
char g_deviceToken[192] = {0};  // empty until the device registers
char g_backendDev[192] = {0};   // dev URL (local IP)
bool g_devMode         = false; // default to prod

bool g_loaded = false;

// Serializes backend persistence with conditional response publication. Never
// held across HTTP. Readers of a request context wait for a complete setter.
StaticSemaphore_t g_backendWriteStorage;
SemaphoreHandle_t g_backendWriteMutex = nullptr;
uint32_t g_backendRevision = 0;
struct BackendWriteGuard {
  BackendWriteGuard() { xSemaphoreTake(g_backendWriteMutex, portMAX_DELAY); }
  ~BackendWriteGuard() { xSemaphoreGive(g_backendWriteMutex); }
};

portMUX_TYPE g_backendMux = portMUX_INITIALIZER_UNLOCKED;
struct BackendGuard {
  BackendGuard() { portENTER_CRITICAL(&g_backendMux); }
  ~BackendGuard() { portEXIT_CRITICAL(&g_backendMux); }
};

// Trim is intentionally NOT done here — leading/trailing whitespace in a
// password is meaningful and there's no way to tell from outside whether the
// user really typed the space. Callers (web UI, settings) should normalize.

void copyClamped(char* dst, size_t dstCap, const char* src) {
  if (!dst || dstCap == 0) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  strncpy(dst, src, dstCap - 1);
  dst[dstCap - 1] = '\0';
}

bool readStringFromNvs(Preferences& p, const char* key, char* dst, size_t dstCap) {
  if (!p.isKey(key)) return false;
  String v = p.getString(key, String());
  if (v.length() == 0) return false;  // Empty == treat as "use macro fallback".
  copyClamped(dst, dstCap, v.c_str());
  return true;
}

bool writeStringToNvs(const char* key, const char* value) {
  Preferences p;
  if (!p.begin(kNs, false)) return false;
  bool ok;
  if (value && value[0] != '\0') {
    ok = (p.putString(key, value) > 0);
  } else {
    // Empty value -> remove the key so the next boot falls back to the macro
    // default. This is what `factoryReset()` relies on.
    ok = p.remove(key) || !p.isKey(key);
  }
  p.end();
  return ok;
}

// Caller holds BackendWriteGuard; keep NVS and logs outside the cache spinlock.
bool writeDeviceToken(const char* token) {
  if (!writeStringToNvs(kKeyDevToken, token)) return false;
  unsigned length;
  {
    BackendGuard guard;
    if (strcmp(g_deviceToken, token ? token : "") != 0) ++g_backendRevision;
    copyClamped(g_deviceToken, sizeof(g_deviceToken), token);
    length = (unsigned)strlen(g_deviceToken);
  }
  Serial.printf("[secrets] setDeviceToken: <set, %u chars>\n", length);
  return true;
}

void slotKeys(int i, char ssidKey[16], char passKey[16]) {
  snprintf(ssidKey, 16, kKeyWifiSsidFmt, i);
  snprintf(passKey, 16, kKeyWifiPassFmt, i);
}

bool persistSlot(int i) {
  if (i < 0 || i >= kMaxWifiNetworks) return false;
  char sk[16], pk[16];
  slotKeys(i, sk, pk);
  bool okS = writeStringToNvs(sk, g_wifiSsid[i]);
  bool okP = writeStringToNvs(pk, g_wifiPass[i]);
  return okS && okP;
}

}  // namespace

void init() {
  if (!g_backendWriteMutex) g_backendWriteMutex = xSemaphoreCreateMutexStatic(&g_backendWriteStorage);
  ++g_backendRevision;
  // Reset RAM cache.
  for (int i = 0; i < kMaxWifiNetworks; i++) {
    g_wifiSsid[i][0] = '\0';
    g_wifiPass[i][0] = '\0';
  }
  g_wifiCount = 0;
  g_wifiLastOk[0] = '\0';
  copyClamped(g_backendDev,  sizeof(g_backendDev),  BACKEND_URL);
  copyClamped(g_backendUrl,  sizeof(g_backendUrl),  BACKEND_URL_PROD);
  g_devMode = false;
  g_deviceToken[0] = '\0';
  copyClamped(g_apiKey,      sizeof(g_apiKey),      AUTH_TOKEN);

  // Read-write open: creates the namespace on first boot. Read-only open fails
  // with NOT_FOUND until at least one key has been written — that broke fresh
  // devices and spammed Preferences errors in the log.
  Preferences p;
  if (!p.begin(kNs, false)) {
    Serial.println("[secrets] NVS open (rw) failed — using compile-time defaults");
    g_wifiLastOk[0] = '\0';
    if (WIFI_SSID[0]) {
      copyClamped(g_wifiSsid[0], sizeof(g_wifiSsid[0]), WIFI_SSID);
      copyClamped(g_wifiPass[0], sizeof(g_wifiPass[0]), WIFI_PASS);
      g_wifiCount = 1;
    }
    g_loaded = true;
    return;
  }

  // ---- Multi-network read --------------------------------------------------
  // Read each slot in order; first empty slot terminates the run (we keep the
  // list packed on disk via removeWifiNetwork()).
  for (int i = 0; i < kMaxWifiNetworks; i++) {
    char sk[16], pk[16];
    slotKeys(i, sk, pk);
    char tmpSsid[33] = {0};
    char tmpPass[65] = {0};
    bool gotS = readStringFromNvs(p, sk, tmpSsid, sizeof(tmpSsid));
    if (!gotS) break;
    readStringFromNvs(p, pk, tmpPass, sizeof(tmpPass));
    copyClamped(g_wifiSsid[g_wifiCount], sizeof(g_wifiSsid[0]), tmpSsid);
    copyClamped(g_wifiPass[g_wifiCount], sizeof(g_wifiPass[0]), tmpPass);
    g_wifiCount++;
  }

  // ---- Migration from legacy single-pair keys -----------------------------
  // If a user upgraded from a build that only knew about wifi_ssid / wifi_pass,
  // promote that pair into slot 0 so they don't lose their saved network.
  if (g_wifiCount == 0) {
    char legacySsid[33] = {0};
    char legacyPass[65] = {0};
    bool gotL = readStringFromNvs(p, kKeyWifiSsid, legacySsid, sizeof(legacySsid));
    if (gotL) {
      readStringFromNvs(p, kKeyWifiPass, legacyPass, sizeof(legacyPass));
      copyClamped(g_wifiSsid[0], sizeof(g_wifiSsid[0]), legacySsid);
      copyClamped(g_wifiPass[0], sizeof(g_wifiPass[0]), legacyPass);
      g_wifiCount = 1;
      Serial.println("[secrets] migrated legacy wifi_ssid/wifi_pass into slot 0");
    }
  }

  // ---- Final compile-time fallback ----------------------------------------
  // First boot: no NVS-stored creds at all. Seed slot 0 with the macro defaults
  // so out-of-the-box devices still try the lab network.
  if (g_wifiCount == 0 && WIFI_SSID[0]) {
    copyClamped(g_wifiSsid[0], sizeof(g_wifiSsid[0]), WIFI_SSID);
    copyClamped(g_wifiPass[0], sizeof(g_wifiPass[0]), WIFI_PASS);
    g_wifiCount = 1;
  }

  bool ovDev = readStringFromNvs(p, kKeyBackendDev, g_backendDev, sizeof(g_backendDev));
  bool ovApi = readStringFromNvs(p, kKeyApi,        g_apiKey,     sizeof(g_apiKey));
  // Per-device token has no compile-time default — empty means "not yet registered".
  (void)readStringFromNvs(p, kKeyDevToken, g_deviceToken, sizeof(g_deviceToken));

  if (p.isKey(kKeyDevMode)) {
    g_devMode = p.getBool(kKeyDevMode, false);
  }

  if (release_policy::productionOnly) {
    // A staging registration token must never be sent to the production API.
    // Clear the old token before persisting production mode. A failed write
    // keeps the migration retryable; RAM still fails closed on this boot.
    if (g_devMode) {
      g_deviceToken[0] = '\0';
      if (p.remove(kKeyDevToken) || !p.isKey(kKeyDevToken)) {
        if (!p.putBool(kKeyDevMode, false))
          Serial.println("[secrets] production migration: mode persistence failed");
      } else {
        Serial.println("[secrets] production migration: token removal failed");
      }
    }
    g_devMode = false;
  }

  if (g_devMode) {
    copyClamped(g_backendUrl, sizeof(g_backendUrl), g_backendDev);
  } else {
    copyClamped(g_backendUrl, sizeof(g_backendUrl), BACKEND_URL_PROD);
  }

  (void)readStringFromNvs(p, kKeyWifiLastOk, g_wifiLastOk, sizeof(g_wifiLastOk));

  p.end();

  // Migration write-back: if we promoted legacy creds (or seeded from macros),
  // persist them into the slot keys so the legacy keys can stop being read.
  // We do this best-effort and only when slot 0 isn't already in NVS.
  {
    Preferences chk;
    if (chk.begin(kNs, false)) {
      char sk[16], pk[16];
      slotKeys(0, sk, pk);
      bool slot0Present = chk.isKey(sk);
      chk.end();
      if (!slot0Present && g_wifiCount > 0) {
        persistSlot(0);
        Serial.println("[secrets] persisted slot 0 (post-migration write-back)");
      }
    }
  }

  Serial.printf("[secrets] init: %d STA cred(s) loaded; primary='%s' last_ok='%s' backend=%s (dev=%d) api=%s (dev_url=%d api=%d)\n",
                g_wifiCount, g_wifiSsid[0], g_wifiLastOk,
                g_backendUrl, g_devMode, g_apiKey[0] ? "<set>" : "<empty>",
                ovDev, ovApi);
  g_loaded = true;
}

bool factoryReset() {
  BackendWriteGuard writeGuard;
  Preferences p;
  if (!p.begin(kNs, false)) return false;
  p.clear();
  p.end();
  // Repopulate from macros so the running session keeps working.
  for (int i = 0; i < kMaxWifiNetworks; i++) {
    g_wifiSsid[i][0] = '\0';
    g_wifiPass[i][0] = '\0';
  }
  g_wifiCount = 0;
  g_wifiLastOk[0] = '\0';
  if (WIFI_SSID[0]) {
    copyClamped(g_wifiSsid[0], sizeof(g_wifiSsid[0]), WIFI_SSID);
    copyClamped(g_wifiPass[0], sizeof(g_wifiPass[0]), WIFI_PASS);
    g_wifiCount = 1;
    persistSlot(0);
  }
  {
    BackendGuard guard;
    copyClamped(g_backendDev,  sizeof(g_backendDev),  BACKEND_URL);
    copyClamped(g_backendUrl,  sizeof(g_backendUrl),
                release_policy::productionOnly ? BACKEND_URL_PROD : BACKEND_URL);
    copyClamped(g_apiKey,      sizeof(g_apiKey),      AUTH_TOKEN);
    g_deviceToken[0] = '\0';  // re-register on next boot (device_id is stable, so backend re-issues)
    g_devMode = !release_policy::productionOnly;
    ++g_backendRevision;
  }
  Serial.println("[secrets] factoryReset: cleared NVS, restored compile-time defaults");
  return true;
}

// ---- Multi-network API ----------------------------------------------------

int wifiNetworkCount() { return g_wifiCount; }

const char* wifiNetworkSSID(int i) {
  if (i < 0 || i >= g_wifiCount) return "";
  return g_wifiSsid[i];
}

const char* wifiNetworkPassword(int i) {
  if (i < 0 || i >= g_wifiCount) return "";
  return g_wifiPass[i];
}

bool addWifiNetwork(const char* ssid, const char* password) {
  if (!ssid || !ssid[0] || strlen(ssid) > 32 || (password && strlen(password) > 64)) return false;
  // If the SSID already exists, treat this as a password update.
  for (int i = 0; i < g_wifiCount; i++) {
    if (strcmp(g_wifiSsid[i], ssid) == 0) {
      char sk[16], pk[16];
      slotKeys(i, sk, pk);
      const bool ok = writeStringToNvs(pk, password ? password : "");
      if (ok) copyClamped(g_wifiPass[i], sizeof(g_wifiPass[0]), password ? password : "");
      Serial.printf("[secrets] addWifiNetwork: updated existing slot %d ssid='%s' (ok=%d)\n",
                    i, g_wifiSsid[i], ok);
      return ok;
    }
  }
  if (g_wifiCount >= kMaxWifiNetworks) {
    Serial.printf("[secrets] addWifiNetwork: full (%d/%d), rejecting '%s'\n",
                  g_wifiCount, kMaxWifiNetworks, ssid);
    return false;
  }
  int slot = g_wifiCount;
  char sk[16], pk[16];
  slotKeys(slot, sk, pk);
  // Write the password first and publish the SSID last. init() discovers slots
  // by SSID, so a failed write cannot create a visible half-saved network.
  if (!writeStringToNvs(pk, password ? password : "")) return false;
  if (!writeStringToNvs(sk, ssid)) {
    (void)writeStringToNvs(pk, "");
    return false;
  }
  copyClamped(g_wifiSsid[slot], sizeof(g_wifiSsid[0]), ssid);
  copyClamped(g_wifiPass[slot], sizeof(g_wifiPass[0]), password ? password : "");
  g_wifiCount++;
  bool ok = true;
  Serial.printf("[secrets] addWifiNetwork: stored slot %d ssid='%s' (ok=%d, total=%d)\n",
                slot, g_wifiSsid[slot], ok, g_wifiCount);
  return ok;
}

bool removeWifiNetwork(int i) {
  if (i < 0 || i >= g_wifiCount) return false;
  if (g_wifiLastOk[0] && strcmp(g_wifiSsid[i], g_wifiLastOk) == 0) {
    g_wifiLastOk[0] = '\0';
    (void)writeStringToNvs(kKeyWifiLastOk, "");
  }
  // Shift the tail down so the on-disk list stays packed (init() reads until
  // the first empty slot).
  for (int j = i; j < g_wifiCount - 1; j++) {
    copyClamped(g_wifiSsid[j], sizeof(g_wifiSsid[0]), g_wifiSsid[j + 1]);
    copyClamped(g_wifiPass[j], sizeof(g_wifiPass[0]), g_wifiPass[j + 1]);
  }
  int last = g_wifiCount - 1;
  g_wifiSsid[last][0] = '\0';
  g_wifiPass[last][0] = '\0';
  g_wifiCount--;

  // Persist: rewrite slots 0..g_wifiCount-1, blank out the now-empty tail
  // (passing "" to writeStringToNvs deletes the key, which is what the next
  // init() needs to stop reading there).
  bool ok = true;
  for (int j = 0; j < g_wifiCount; j++) ok = persistSlot(j) && ok;
  for (int j = g_wifiCount; j < kMaxWifiNetworks; j++) {
    char sk[16], pk[16];
    slotKeys(j, sk, pk);
    writeStringToNvs(sk, "");
    writeStringToNvs(pk, "");
  }
  Serial.printf("[secrets] removeWifiNetwork(%d): ok=%d remaining=%d\n", i, ok, g_wifiCount);
  return ok;
}

bool promoteWifiNetworkToFront(const char* ssid) {
  if (!ssid || !ssid[0]) return false;
  int idx = -1;
  for (int i = 0; i < g_wifiCount; i++) {
    if (strcmp(g_wifiSsid[i], ssid) == 0) {
      idx = i;
      break;
    }
  }
  if (idx < 0) return false;
  while (idx > 0) {
    char ts[sizeof(g_wifiSsid[0])];
    char tp[sizeof(g_wifiPass[0])];
    copyClamped(ts, sizeof(ts), g_wifiSsid[idx]);
    copyClamped(tp, sizeof(tp), g_wifiPass[idx]);
    copyClamped(g_wifiSsid[idx], sizeof(g_wifiSsid[0]), g_wifiSsid[idx - 1]);
    copyClamped(g_wifiPass[idx], sizeof(g_wifiPass[0]), g_wifiPass[idx - 1]);
    copyClamped(g_wifiSsid[idx - 1], sizeof(g_wifiSsid[0]), ts);
    copyClamped(g_wifiPass[idx - 1], sizeof(g_wifiPass[0]), tp);
    idx--;
  }
  bool ok = true;
  for (int j = 0; j < g_wifiCount; j++) ok = persistSlot(j) && ok;
  Serial.printf("[secrets] promoteToFront: ssid='%s' ok=%d\n", ssid, ok);
  return ok;
}

const char* lastConnectedWifi() { return g_wifiLastOk; }

bool setLastConnectedWifi(const char* ssid) {
  if (!ssid || !ssid[0]) {
    g_wifiLastOk[0] = '\0';
    return writeStringToNvs(kKeyWifiLastOk, "");
  }
  copyClamped(g_wifiLastOk, sizeof(g_wifiLastOk), ssid);
  return writeStringToNvs(kKeyWifiLastOk, ssid);
}

const char* wifiSSID()     { return g_wifiCount > 0 ? g_wifiSsid[0] : ""; }
const char* wifiPassword() { return g_wifiCount > 0 ? g_wifiPass[0] : ""; }
BackendConfig backendConfig() {
  BackendGuard guard;
  BackendConfig result;
  memcpy(result.url, g_backendUrl, sizeof(result.url));
  memcpy(result.apiKey, g_apiKey, sizeof(result.apiKey));
  return result;
}

bool hasDeviceToken() { BackendGuard guard; return g_deviceToken[0] != '\0'; }

BackendRequest backendRequest() {
  BackendWriteGuard writeGuard;
  BackendGuard guard;
  BackendRequest result;
  memcpy(result.backend.url, g_backendUrl, sizeof(result.backend.url));
  memcpy(result.backend.apiKey, g_apiKey, sizeof(result.backend.apiKey));
  memcpy(result.token, g_deviceToken, sizeof(result.token));
  result.revision = g_backendRevision;
  return result;
}

bool setDeviceTokenFor(const BackendRequest& request, const char* token) {
  BackendWriteGuard writeGuard;
  { BackendGuard guard; if (request.revision != g_backendRevision) return false; }
  return writeDeviceToken(token);
}

bool setWifi(const char* ssid, const char* password) {
  // Old single-credential entry point. Just funnel into the multi API so the
  // semantics (replace if SSID matches, otherwise append) are consistent.
  return addWifiNetwork(ssid, password);
}

bool setBackendUrl(const char* url) {
  if (!url) return false;
  if (release_policy::productionOnly && strcmp(url, BACKEND_URL_PROD) != 0) return false;
  BackendWriteGuard writeGuard;
  bool changed;
  { BackendGuard guard; changed = strcmp(g_backendUrl, url) != 0; }
  if (changed && !writeDeviceToken("")) return false;
  // In dev mode, setting the backend URL updates the dev URL.
  // In prod mode, the prod URL is fixed at compile time.
  const bool dev = isDevMode();
  if (dev && !writeStringToNvs(kKeyBackendDev, url)) return false;
  char published[sizeof(g_backendUrl)];
  {
    BackendGuard guard;
    if (dev) copyClamped(g_backendDev, sizeof(g_backendDev), url);
    copyClamped(g_backendUrl, sizeof(g_backendUrl), url);
    if (changed) ++g_backendRevision;
    memcpy(published, g_backendUrl, sizeof(published));
  }
  Serial.printf("[secrets] setBackendUrl: %s\n", published);
  return true;
}

uint32_t apiKeyRevision() { return g_apiKeyRevision.load(); }

bool setApiKey(const char* key) {
  if (!key) return false;
  BackendWriteGuard writeGuard;
  bool changed;
  { BackendGuard guard; changed = strcmp(g_apiKey, key) != 0; }
  if (changed && !writeDeviceToken("")) return false;
  if (!writeStringToNvs(kKeyApi, key)) return false;
  unsigned length;
  {
    BackendGuard guard;
    copyClamped(g_apiKey, sizeof(g_apiKey), key);
    if (changed) { ++g_apiKeyRevision; ++g_backendRevision; }
    length = (unsigned)strlen(g_apiKey);
  }
  Serial.printf("[secrets] setApiKey: <set, %u chars>\n", length);
  return true;
}

bool setDeviceToken(const char* token) {
  BackendWriteGuard writeGuard;
  return writeDeviceToken(token);
}

bool isDevMode() { BackendGuard guard; return g_devMode; }

bool setDevMode(bool dev) {
  if (release_policy::productionOnly && dev) return false;
  BackendWriteGuard writeGuard;
  const bool changed = isDevMode() != dev;
  if (changed && !writeDeviceToken("")) return false;
  Preferences p;
  if (!p.begin(kNs, false)) return false;
  p.putBool(kKeyDevMode, dev);
  p.end();

  char published[sizeof(g_backendUrl)];
  {
    BackendGuard guard;
    if (changed || strcmp(g_backendUrl, dev ? g_backendDev : BACKEND_URL_PROD) != 0) ++g_backendRevision;
    g_devMode = dev;
    if (dev) {
      copyClamped(g_backendUrl, sizeof(g_backendUrl), g_backendDev);
    } else {
      copyClamped(g_backendUrl, sizeof(g_backendUrl), BACKEND_URL_PROD);
    }
    memcpy(published, g_backendUrl, sizeof(published));
  }
  Serial.printf("[secrets] setDevMode: %s -> %s\n", dev ? "DEV" : "PROD", published);
  return true;
}

}  // namespace secrets
}  // namespace core
