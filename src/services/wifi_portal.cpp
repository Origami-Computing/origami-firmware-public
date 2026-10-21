#include "services/wifi_portal.h"
#include <Arduino.h>
#include <WiFi.h>
#include "hal/hal_wifi.h"
#include "core/secrets.h"
#include "services/wifi_provision.h"
#include <WebServer.h>
#include <DNSServer.h>
#include <esp_wifi.h>

namespace services { namespace wifi_portal {
namespace {
bool s_open = false;
bool s_ready = false;
// Use pointers with lazy initialization to avoid static construction issues
static WebServer* server = nullptr;
static DNSServer* dnsServer = nullptr;
static bool s_running = false;
static unsigned long s_lastHandle = 0;
static const unsigned long HANDLE_INTERVAL_MS = 50;  // Throttle to 20Hz max

// HTML page for WiFi configuration (stored in flash)
static const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Origami WiFi Setup</title>
  <style>
    * { box-sizing: border-box; font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif; }
    body { margin: 0; padding: 20px; background: #1a1a2e; color: #eee; min-height: 100vh; }
    .container { max-width: 400px; margin: 0 auto; }
    h1 { color: #00d4ff; font-size: 24px; margin-bottom: 20px; }
    .card { background: #16213e; border-radius: 12px; padding: 20px; margin-bottom: 16px; }
    .status { padding: 12px; border-radius: 8px; margin-bottom: 16px; }
    .status.ap { background: #0f3460; border-left: 4px solid #00d4ff; }
    .status.connected { background: #1b4332; border-left: 4px solid #40916c; }
    label { display: block; margin-bottom: 6px; color: #aaa; font-size: 14px; }
    input { width: 100%; padding: 12px; border: 1px solid #333; border-radius: 8px; 
            background: #0f0f23; color: #fff; font-size: 16px; margin-bottom: 16px; }
    input:focus { outline: none; border-color: #00d4ff; }
    button { width: 100%; padding: 14px; border: none; border-radius: 8px; font-size: 16px; cursor: pointer; }
    .btn-primary { background: #00d4ff; color: #000; font-weight: 600; }
    .btn-secondary { background: #333; color: #fff; margin-top: 8px; }
    .network-list { max-height: 200px; overflow-y: auto; }
    .network { padding: 12px; background: #0f0f23; border-radius: 8px; margin-bottom: 8px; cursor: pointer; }
    .network:hover { background: #1a1a3e; }
    .saved { padding: 10px; background: #0f0f23; border-radius: 8px; margin-bottom: 6px;
             display: flex; justify-content: space-between; align-items: center; }
    .saved .ssid { color: #fff; }
    .saved .forget { background: #4a1515; color: #ff9999; border: none; border-radius: 6px;
                     padding: 6px 10px; font-size: 13px; cursor: pointer; }
    .saved .forget:hover { background: #6a1515; }
    .hidden { display: none; }
    .msg { padding: 10px; border-radius: 6px; margin-top: 10px; font-size: 14px; }
    .msg.error { background: #4a1515; color: #ff6b6b; }
    .msg.success { background: #1b4332; color: #40916c; }
  </style>
</head>
<body>
  <div class="container">
    <h1>Origami WiFi Setup</h1>
    <div id="status" class="status ap">
      <strong>Mode:</strong> <span id="mode">Access Point</span><br>
      <strong>IP:</strong> <span id="ip">192.168.4.1</span>
    </div>
    <div class="card">
      <label>Saved Networks <span id="savedCount" style="color:#aaa;font-weight:normal;"></span></label>
      <div id="saved">Loading...</div>
    </div>
    <div class="card">
      <label>Available Networks</label>
      <div id="networks" class="network-list">Click Scan to find networks</div>
      <button class="btn-secondary" onclick="scan()" id="scanBtn">Scan for Networks</button>
    </div>
    <div class="card">
      <label for="ssid">Add Network (SSID)</label>
      <input type="text" id="ssid" placeholder="Enter network name">
      <label for="pass">Password</label>
      <input type="password" id="pass" placeholder="Enter password">
      <button class="btn-primary" onclick="connect()">Save &amp; Connect</button>
      <div id="msg" class="msg hidden"></div>
    </div>
    <div class="card">
      <label for="apikey">API Key</label>
      <input type="text" id="apikey" placeholder="Paste your API key">
      <button class="btn-primary" onclick="saveKey()">Save API Key</button>
      <div id="keymsg" class="msg hidden"></div>
    </div>
  </div>
  <script>
    function scan() {
      document.getElementById('scanBtn').textContent = 'Scanning...';
      document.getElementById('networks').innerHTML = 'Scanning...';
      fetch('/scan').then(r => r.json()).then(data => {
        document.getElementById('scanBtn').textContent = 'Scan for Networks';
        if (data.networks && data.networks.length > 0) {
          const container = document.getElementById('networks');
          container.replaceChildren();
          data.networks.forEach(n => {
            const row = document.createElement('div');
            row.className = 'network';
            row.textContent = n.ssid + ' (' + n.rssi + 'dBm)';
            row.addEventListener('click', () => {
              document.getElementById('ssid').value = n.ssid;
            });
            container.appendChild(row);
          });
        } else {
          document.getElementById('networks').innerHTML = 'No networks found';
        }
      }).catch(e => {
        document.getElementById('scanBtn').textContent = 'Scan for Networks';
        document.getElementById('networks').innerHTML = 'Scan failed';
      });
    }
    function connect() {
      const ssid = document.getElementById('ssid').value;
      const pass = document.getElementById('pass').value;
      const msg = document.getElementById('msg');
      if (!ssid) { msg.className = 'msg error'; msg.textContent = 'Enter network name'; msg.classList.remove('hidden'); return; }
      msg.className = 'msg'; msg.textContent = 'Connecting...'; msg.classList.remove('hidden');
      fetch('/connect', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'},
        body:'ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass)})
        .then(r => r.json()).then(data => {
          if (data.success) { msg.className = 'msg success'; msg.textContent = 'Connecting to ' + ssid + '...'; setTimeout(updateStatus, 5000); }
          else { msg.className = 'msg error'; msg.textContent = data.error || 'Failed'; }
        }).catch(e => { msg.className = 'msg error'; msg.textContent = 'Request failed'; });
    }
    function updateStatus() {
      fetch('/status').then(r => r.json()).then(data => {
        document.getElementById('mode').textContent = data.mode;
        document.getElementById('ip').textContent = data.ip;
        document.getElementById('status').className = 'status ' + (data.staConnected ? 'connected' : 'ap');
        if (data.staConnected) {
          document.getElementById('msg').className = 'msg success';
          document.getElementById('msg').textContent = 'Connected to ' + (data.staSsid || '') + ' (' + data.staIp + ')';
          document.getElementById('msg').classList.remove('hidden');
        }
      }).catch(e => {});
    }
    function refreshSaved() {
      fetch('/networks').then(r => r.json()).then(data => {
        const max = data.max || 4;
        const nets = data.networks || [];
        document.getElementById('savedCount').textContent = '(' + nets.length + '/' + max + ')';
        if (nets.length === 0) {
          document.getElementById('saved').innerHTML = '<div style="color:#aaa;">No saved networks</div>';
          return;
        }
        const container = document.getElementById('saved');
        container.replaceChildren();
        nets.forEach((n, i) => {
          const row = document.createElement('div');
          row.className = 'saved';
          const label = document.createElement('span');
          label.className = 'ssid';
          label.textContent = n.ssid;
          const button = document.createElement('button');
          button.className = 'forget';
          button.textContent = 'Forget';
          button.addEventListener('click', () => forget(i, n.ssid));
          row.appendChild(label);
          row.appendChild(button);
          container.appendChild(row);
        });
      }).catch(e => {
        document.getElementById('saved').innerHTML = '<div style="color:#ff6b6b;">Failed to load</div>';
      });
    }
    function forget(idx, ssid) {
      if (!confirm('Forget "' + ssid + '"?')) return;
      fetch('/forget', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'},
        body:'index='+encodeURIComponent(idx)})
        .then(r => r.json()).then(data => { refreshSaved(); }).catch(e => {});
    }
    function saveKey() {
      const key = document.getElementById('apikey').value;
      const msg = document.getElementById('keymsg');
      if (!key) { msg.className = 'msg error'; msg.textContent = 'Enter an API key'; msg.classList.remove('hidden'); return; }
      fetch('/settings', {method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'},
        body:'api_key='+encodeURIComponent(key)})
        .then(r => r.json()).then(data => {
          if (data.success) { msg.className = 'msg success'; msg.textContent = 'API key saved'; }
          else { msg.className = 'msg error'; msg.textContent = data.error || 'Failed'; }
          msg.classList.remove('hidden');
        }).catch(e => { msg.className = 'msg error'; msg.textContent = 'Request failed'; msg.classList.remove('hidden'); });
    }
    updateStatus();
    refreshSaved();
    setInterval(updateStatus, 10000);
    setInterval(refreshSaved, 15000);
  </script>
</body>
</html>
)rawliteral";

// Handle root page
static void handleRoot() {
  server->send(200, "text/html", INDEX_HTML);
}

// Handle scan request
static void handleScan() {
  Serial.println("[webserver] Scanning...");
  
  wifi_mode_t currentMode;
  esp_wifi_get_mode(&currentMode);
  
  // Switch to APSTA if needed for scanning
  bool switchedMode = false;
  if (currentMode == WIFI_MODE_AP) {
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    switchedMode = true;
    delay(100);
  }
  
  wifi_ap_record_t results[15];
  uint16_t count = 15;
  
  String json = "{\"networks\":[";
  
  if (hal::wifi::scan(results, &count, 15)) {
    for (int i = 0; i < count; i++) {
      if (i > 0) json += ",";
      json += "{\"ssid\":\"";
      for (int j = 0; results[i].ssid[j] && j < 32; j++) {
        char c = results[i].ssid[j];
        if (c == '"') json += "\\\"";
        else if (c == '\\') json += "\\\\";
        else if (c >= 32 && c < 127) json += c;
      }
      json += "\",\"rssi\":";
      json += String(results[i].rssi);
      json += "}";
    }
  }
  
  json += "]}";
  
  if (switchedMode) {
    esp_wifi_set_mode(WIFI_MODE_AP);
  }
  
  server->send(200, "application/json", json);
}

// Handle connect request — adds (or updates) the network and triggers a join.
// Does NOT clear other saved networks; this is the "phone-style" behavior the
// user wanted (multiple known APs, auto-pick best).
static void handleConnect() {
  String ssid = server->arg("ssid");
  String pass = server->arg("pass");

  Serial.printf("[webserver] Connect/Add: '%s'\n", ssid.c_str());

  if (ssid.length() == 0) {
    server->send(200, "application/json", "{\"success\":false,\"error\":\"SSID required\"}");
    return;
  }

  core::wifi_qr::Credentials creds = {};
  strncpy(creds.ssid, ssid.c_str(), sizeof(creds.ssid) - 1);
  strncpy(creds.pass, pass.c_str(), sizeof(creds.pass) - 1);

  // keepApUp=true: stay in APSTA so this HTTP response actually reaches the
  // phone (which is connected to our soft-AP). The portal app reports join
  // status via /status polling, and tears the AP down on exit.
  server->send(200, "application/json",
               services::wifi_provision::saveAndConnect(creds, /*keepApUp=*/true)
                   ? "{\"success\":true,\"persisted\":true}"
                   : "{\"success\":false,\"error\":\"Saved networks full (4 max). Forget one first.\"}");
}

// Handle saved-networks list (GET)
static void handleNetworks() {
  String json = "{\"max\":";
  json += String(core::secrets::kMaxWifiNetworks);
  json += ",\"networks\":[";
  const int n = core::secrets::wifiNetworkCount();
  for (int i = 0; i < n; i++) {
    if (i > 0) json += ",";
    const char* s = core::secrets::wifiNetworkSSID(i);
    json += "{\"i\":";
    json += String(i);
    json += ",\"ssid\":\"";
    for (const char* p = s; *p; p++) {
      if (*p == '"') json += "\\\"";
      else if (*p == '\\') json += "\\\\";
      else if ((unsigned char)*p >= 32 && (unsigned char)*p < 127) json += *p;
    }
    json += "\"}";
  }
  json += "]}";
  server->send(200, "application/json", json);
}

// Handle forget request (POST index=<int>)
static void handleForget() {
  String idxStr = server->arg("index");
  if (idxStr.length() == 0) {
    server->send(200, "application/json", "{\"success\":false,\"error\":\"index required\"}");
    return;
  }
  int idx = idxStr.toInt();
  Serial.printf("[webserver] Forget: index=%d\n", idx);
  if (!core::secrets::removeWifiNetwork(idx)) {
    server->send(200, "application/json", "{\"success\":false,\"error\":\"remove failed\"}");
    return;
  }
  // Re-sync the radio's WiFiMulti list with the new persistent state. We rebuild
  // from scratch so there's no chance of a stale credential lingering after a
  // forget (the WiFiMulti instance has no remove API).
  hal::wifi::reloadCredentialsFromSecrets();
  server->send(200, "application/json", "{\"success\":true}");
}

// Handle status request
static void handleStatus() {
  wifi_mode_t mode;
  esp_wifi_get_mode(&mode);
  
  const char* modeStr = "Unknown";
  switch (mode) {
    case WIFI_MODE_STA: modeStr = "Station"; break;
    case WIFI_MODE_AP: modeStr = "Access Point"; break;
    case WIFI_MODE_APSTA: modeStr = "AP + Station"; break;
    default: break;
  }
  
  esp_ip4_addr_t apIp = hal::wifi::getAPIP();
  esp_ip4_addr_t staIp = hal::wifi::getSTAIP();

  char json[320];
  snprintf(json, sizeof(json),
    "{\"mode\":\"%s\",\"ip\":\"" IPSTR "\",\"staConnected\":%s,\"staIp\":\"" IPSTR "\",\"staSsid\":\"%s\"}",
    modeStr, IP2STR(&apIp), hal::wifi::connected() ? "true" : "false", IP2STR(&staIp),
    hal::wifi::getSTASSID());
  
  server->send(200, "application/json", json);
}

// Handle settings (API key)
static void handleSettings() {
  String apiKey = server->arg("api_key");
  if (apiKey.length() == 0) {
    server->send(200, "application/json", "{\"success\":false,\"error\":\"API key required\"}");
    return;
  }
  bool ok = core::secrets::setApiKey(apiKey.c_str());
  if (ok) {
    server->send(200, "application/json", "{\"success\":true}");
  } else {
    server->send(200, "application/json", "{\"success\":false,\"error\":\"NVS write failed\"}");
  }
}

// Captive portal redirect
static void handleNotFound() {
  server->sendHeader("Location", "http://192.168.4.1/", true);
  server->send(302, "text/plain", "");
}

bool startHttp() {
  if (s_running) return true;
  
  Serial.println("[webserver] Starting...");
  
  // Create objects only when needed (lazy initialization)
  if (!server) {
    server = new WebServer(80);
  }
  if (!dnsServer) {
    dnsServer = new DNSServer();
  }
  
  // Setup routes
  server->on("/", HTTP_GET, handleRoot);
  server->on("/scan", HTTP_GET, handleScan);
  server->on("/connect", HTTP_POST, handleConnect);
  server->on("/networks", HTTP_GET, handleNetworks);
  server->on("/forget", HTTP_POST, handleForget);
  server->on("/settings", HTTP_POST, handleSettings);
  server->on("/status", HTTP_GET, handleStatus);
  server->onNotFound(handleNotFound);
  
  // Start DNS server for captive portal
  dnsServer->start(53, "*", IPAddress(192, 168, 4, 1));
  
  // Start web server
  server->begin();
  
  s_running = true;
  s_lastHandle = millis();
  Serial.println("[webserver] Started on http://192.168.4.1");
  
  return true;
}

void pollHttp() {
  if (!s_running || !server || !dnsServer) return;
  
  // Throttle handle calls to reduce interference with other tasks
  unsigned long now = millis();
  if (now - s_lastHandle < HANDLE_INTERVAL_MS) return;
  s_lastHandle = now;
  
  dnsServer->processNextRequest();
  server->handleClient();
}

void stopHttp() {
  if (!s_running) return;
  
  if (server) {
    server->stop();
    delete server;
    server = nullptr;
  }
  if (dnsServer) {
    dnsServer->stop();
    delete dnsServer;
    dnsServer = nullptr;
  }
  
  s_running = false;
  Serial.println("[webserver] Stopped");
}

} // namespace

bool open() {
  if (s_open) return s_ready;
  s_open = true;
  // Block any deferred STA restore that some other app scheduled; otherwise
  // the next main-loop iteration could `WiFi.mode(STA)` us out of soft-AP
  // before the phone even sees the SSID.
  hal::wifi::setWifiRestoreDeferred(true);

  WiFi.disconnect(true);
  if (!hal::wifi::setMode(WIFI_MODE_AP)) {
    Serial.println("[wifi-portal] setMode(AP) FAILED");
  }

  const bool apOk = hal::wifi::startAP();
  if (apOk) {
    Serial.printf("[wifi-portal] AP up: %s @ %s\n",
                  hal::wifi::getAPSSID(),
                  WiFi.softAPIP().toString().c_str());
  } else {
    Serial.println("[wifi-portal] AP FAILED");
  }
  hal::wifi::applyModemSleepPolicy();

  const bool serverOk = startHttp();
  Serial.println("[wifi-portal] webserver started on 192.168.4.1");
  s_ready = apOk && serverOk;
  return s_ready;
}

void close() {
  if (!s_open) return;
  s_open = false;
  s_ready = false;
  stopHttp();
  hal::wifi::stopAP();
  hal::wifi::setWifiRestoreDeferred(false);
  if (hal::wifi::staCredentialCount() > 0) {
    hal::wifi::requestConnect(WIFI_MODE_STA);
    Serial.println("[wifi-portal] requested STA reconnect");
  } else {
    // Reuse the manager's settled WiFi-off -> OfflineBle transition. Calling
    // WiFi.mode(STA) inline here would start WiFi with nothing to join and race
    // BLE reconciliation just like the QR-exit crash.
    hal::wifi::requestCredentialsChanged();
    Serial.println("[wifi-portal] requested offline transition");
  }
}


void poll() { if (s_open) pollHttp(); }
Status status() {
  Status result{};
  result.clients = hal::wifi::getAPClientCount();
  result.stationConnected = hal::wifi::connected();
  if (result.stationConnected) {
    const auto ip = hal::wifi::getSTAIP();
    snprintf(result.stationAddress, sizeof(result.stationAddress), "%d.%d.%d.%d", IP2STR(&ip));
  }
  return result;
}
const char* networkName() { return hal::wifi::getAPSSID(); }
const char* password() { return "origami123"; } // Existing HAL default.
} }
