#include "services/device_fs.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <SD.h>
#include <WiFiClientSecure.h>
#include "services/tls_trust.h"

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "core/device_id.h"
#include "core/device_agent_text.h"
#include "core/menu_config.h"
#include "services/net_poll.h"
#include "services/uploads.h"
#include "services/agent_coordination.h"
#include "core/secrets.h"
#include "core/watchdog.h"
#include "core/telemetry.h"
#include "hal/hal_bus.h"

// SD file sync executor. See device_fs.h for the op protocol. Executor work
// runs on net_poll (Core 0); request/status entry points share s_statusMux.
// SD access is always wrapped in
// hal::bus::lockSpi/unlockSpi and NEVER held across network I/O (the SD, TFT,
// and camera share one HSPI bus — same rule the upload_queue follows).

namespace services {
namespace device_fs {

namespace {

// Keep TLS writes below a TCP segment; larger bursts exhaust fragmented
// internal lwIP buffers while the agent WebSocket is active.
constexpr size_t kChunk = 1024;

// HTTPClient otherwise submits an entire directory/manifest JSON body in one
// socket write. Keep its header/body writes within the same lwIP budget as file
// pushes, and service the subscribed worker's watchdog between SDK calls.
// Cancellation/deadline are cooperative: an in-progress SDK write must return
// before this adapter can observe them. HTTPClient retains response handling.
template<class Transport>
class JsonClient final : public Transport {
public:
  size_t write(const uint8_t* data, size_t size) override {
    if (!started_) { started_ = true; startedMs_ = millis(); }
    size_t sent = 0;
    while (sent < size) {
      core::watchdog::feed();
      if (services::net_poll::stopRequested() || uint32_t(millis() - startedMs_) >= 30000u) break;
      const size_t bytes = size - sent < kChunk ? size - sent : kChunk;
      const size_t written = Transport::write(data + sent, bytes);
      core::watchdog::feed();
      if (!written || written > bytes) break;
      sent += written;
    }
    return sent;
  }
private:
  bool started_ = false;
  uint32_t startedMs_ = 0;
};

// Operation batches and manifests can retain many small JSON allocations for
// the entire transfer. Default malloc puts these in scarce internal RAM, which
// is also needed for TLS DMA and audio. JSON has no DMA/internal-RAM constraint.
class JsonMemory final : public ArduinoJson::Allocator {
public:
  void* allocate(size_t n) override {
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
  void deallocate(void* p) override { heap_caps_free(p); }
  void* reallocate(void* p, size_t n) override {
    return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }
};
JsonMemory s_jsonMemory;

static bool s_syncRequested = false; // Protected together with status by s_statusMux.
static portMUX_TYPE s_statusMux = portMUX_INITIALIZER_UNLOCKED;
static SyncStatus s_status = {
    false, false, true, 0, 1, 0, "idle", "Ready",
};

void setStatusLocked(const char* phase, const char* detail, uint32_t value, uint32_t max,
               bool active, bool done, bool ok) {
  if (done && s_syncRequested) return; // Older work cannot finish a pending request.
  s_status.active = active;
  s_status.done = done;
  s_status.ok = ok;
  s_status.value = value;
  s_status.max = max > 0 ? max : 1;
  s_status.updated_ms = millis();
  strlcpy(s_status.phase, phase ? phase : "", sizeof(s_status.phase));
  strlcpy(s_status.detail, detail ? detail : "", sizeof(s_status.detail));
}
void setStatus(const char* phase, const char* detail, uint32_t value, uint32_t max,
               bool active = true, bool done = false, bool ok = true) {
  portENTER_CRITICAL(&s_statusMux);
  setStatusLocked(phase, detail, value, max, active, done, ok);
  portEXIT_CRITICAL(&s_statusMux);
}

void finishPollStatus(uint32_t opTotal, uint32_t pending) {
  portENTER_CRITICAL(&s_statusMux);
  if (opTotal == 0 && pending > 0) {
    // Server retry leases are still active work, even in an empty poll.
    setStatusLocked("Sync retrying", "Waiting for transfers", 0, pending, true, false, true);
  } else if (s_status.active) {
    if (opTotal == 0)
      setStatusLocked("Sync complete", "Up to date", 1, 1, false, true, true);
    else
      setStatusLocked("Syncing", "Finishing", opTotal, opTotal, true, false, true);
  }
  portEXIT_CRITICAL(&s_statusMux);
}

bool hasHiddenSegment(const String& path) {
  return path.startsWith("/.") || path.indexOf("/.") >= 0;
}

// Folders we must never touch: upload_queue is owned by the timeline upload
// worker, and dot-prefixed folders are host OS metadata, not user files.
bool isExcluded(const String& path) {
  return path == "/telemetry_queue" || path.startsWith("/telemetry_queue/") ||
         path == "/upload_queue" || path.startsWith("/upload_queue/") ||
         path == "/System Volume Information" || path.startsWith("/System Volume Information/") ||
         hasHiddenSegment(path);
}

// Skip host OS metadata and hidden files/folders.
bool skipName(const char* name) {
  return name[0] == '.';
}

String basename(const char* name) {
  const char* slash = strrchr(name, '/');
  return String(slash ? slash + 1 : name);
}

String joinPath(const String& dir, const String& base) {
  if (dir == "/") return "/" + base;
  return dir + "/" + base;
}

// Percent-encode a path for use in a query-string value.
String urlEncode(const String& s) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  out.reserve(s.length() * 3);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '/' || c == '.' || c == '-' || c == '_' || c == '~') {
      out += c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0xF];
      out += hex[c & 0xF];
    }
  }
  return out;
}

bool ok2xx(int code) { return code >= 200 && code < 300; }

// Attach device auth. Prefers the per-device bearer token (device-fs endpoints
// require it); X-Device-Id is informational.
void addAuth(HTTPClient& http, const core::secrets::BackendRequest& request) {
  http.addHeader("X-Device-Id", core::device::id());
  const char* token = request.token;
  if (token[0]) http.addHeader("Authorization", String("Bearer ") + token);
}

// GET {backend}{path} → parse JSON into `out`. Returns HTTP status (or -1).
int httpGetJson(const char* path, JsonDocument& out) {
  const auto request = core::secrets::backendRequest();
  String url = String(request.backend.url) + String(path);
  const bool https = url.startsWith("https://");
  WiFiClientSecure sec; WiFiClient plain;
  if (https) { services::tls::useRootBundle(sec); sec.setHandshakeTimeout(8); }
  WiFiClient& wc = https ? static_cast<WiFiClient&>(sec) : static_cast<WiFiClient&>(plain);

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(6000);
  if (!http.begin(wc, url)) return -1;
  addAuth(http, request);
  const int code = http.GET();
  if (ok2xx(code)) {
    if (deserializeJson(out, http.getString())) { http.end(); return -1; }
  }
  http.end();
  if (code == 401) core::secrets::setDeviceTokenFor(request, "");
  return code;
}

// POST a JSON body to {backend}{path}. Returns HTTP status (or -1).
int httpPostJson(const String& path, const String& body) {
  const auto request = core::secrets::backendRequest();
  String url = String(request.backend.url) + path;
  const bool https = url.startsWith("https://");
  JsonClient<WiFiClientSecure> sec; JsonClient<WiFiClient> plain;
  if (https) { services::tls::useRootBundle(sec); sec.setHandshakeTimeout(8); }
  WiFiClient& wc = https ? static_cast<WiFiClient&>(sec) : static_cast<WiFiClient&>(plain);

  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(8000);
  if (!http.begin(wc, url)) return -1;
  addAuth(http, request);
  http.addHeader("Content-Type", "application/json");
  core::watchdog::feed();
  const int code = http.POST(const_cast<String&>(body));
  core::watchdog::feed();
  http.end();
  if (code == 401) core::secrets::setDeviceTokenFor(request, "");
  return code;
}

// Ack an op. `mtime` (epoch secs) is echoed for pulls so the server can align
// its sync baseline to the write time the device produced.
void reportResult(const char* opId, bool ok, uint32_t mtime = 0, const char* err = nullptr) {
  if (!opId || !opId[0]) return;
  JsonDocument doc(&s_jsonMemory);
  doc["ok"] = ok;
  doc["mtime"] = mtime;
  if (!ok && err) doc["error"] = err;
  String body; serializeJson(doc, body);
  String path = String(DEVICE_FS_OPS_PATH) + "/" + opId + "/result";
  httpPostJson(path, body);
}

// ── SD helpers (each discrete SD call under the SPI lock) ────────────────────

// Recursively create parent directories of `path` (e.g. for /a/b/c.lua make
// /a and /a/b). Locks per mkdir.
void ensureParentDirs(const String& path) {
  int slash = path.indexOf('/', 1);
  while (slash > 0) {
    String dir = path.substring(0, slash);
    if (dir.length()) {
      hal::bus::lockSpi();
      if (!SD.exists(dir)) SD.mkdir(dir);
      hal::bus::unlockSpi();
    }
    slash = path.indexOf('/', slash + 1);
  }
}

// Remove a file or a whole directory subtree.
bool removeRecursive(const String& path) {
  bool isDir = false;
  hal::bus::lockSpi();
  File f = SD.open(path);
  if (f) { isDir = f.isDirectory(); f.close(); }
  hal::bus::unlockSpi();

  if (!isDir) {
    hal::bus::lockSpi();
    bool ok = SD.remove(path);
    hal::bus::unlockSpi();
    return ok;
  }

  // Collect children (under lock), then recurse (outside lock).
  std::vector<String> children;
  hal::bus::lockSpi();
  File d = SD.open(path);
  if (d && d.isDirectory()) {
    for (File c = d.openNextFile(); c; c = d.openNextFile()) {
      children.push_back(joinPath(path, basename(c.name())));
      c.close();
    }
  }
  if (d) d.close();
  hal::bus::unlockSpi();

  bool childrenRemoved = true;
  for (const String& child : children) {
    if (!removeRecursive(child)) childrenRemoved = false;
  }

  if (!childrenRemoved) return false;

  hal::bus::lockSpi();
  bool ok = SD.rmdir(path);
  hal::bus::unlockSpi();
  return ok;
}

// ── ops ──────────────────────────────────────────────────────────────────────

struct Child { String path; bool isDir; uint32_t size; uint32_t mtime; };

// One directory's immediate children, gathered under a single lock hold so a
// concurrent display flush can't interleave with the dir iteration.
bool listDir(const String& dir, std::vector<Child>& out) {
  if (!hal::bus::lockSpi()) return false;
  File d = SD.open(dir);
  const bool readable = d && d.isDirectory();
  if (d && d.isDirectory()) {
    for (File c = d.openNextFile(); c; c = d.openNextFile()) {
      const char* nm = c.name();
      String base = basename(nm);
      if (!skipName(base.c_str())) {
        out.push_back({ joinPath(dir, base), c.isDirectory(),
                        (uint32_t)c.size(), (uint32_t)c.getLastWrite() });
      }
      c.close();
    }
  }
  if (d) d.close();
  hal::bus::unlockSpi();
  return readable;
}

// Walk the tree from `dir`, appending files (not dirs) to `entries`, excluding
// /upload_queue. Bounded by DEVICE_FS_MANIFEST_MAX.
bool walkInto(const String& dir, JsonArray entries, int& count) {
  if (count >= DEVICE_FS_MANIFEST_MAX || services::net_poll::stopRequested()) return false;
  std::vector<Child> children;
  if (!listDir(dir, children)) return false;
  bool complete = true;
  for (const Child& c : children) {
    if (isExcluded(c.path)) continue;
    if (c.isDir) {
      if (!walkInto(c.path, entries, count)) complete = false;
    } else if (count < DEVICE_FS_MANIFEST_MAX) {
      JsonObject e = entries.add<JsonObject>();
      e["path"]  = c.path;
      e["size"]  = c.size;
      e["mtime"] = c.mtime;
      count++;
      if ((count & 7) == 0) {
        setStatus("Walking SD", c.path.c_str(), (uint32_t)count, DEVICE_FS_MANIFEST_MAX);
      }
    }
  }
  core::watchdog::feed();
  vTaskDelay(0);
  return complete && count < DEVICE_FS_MANIFEST_MAX && !services::net_poll::stopRequested();
}

void doManifest(const char* opId) {
  setStatus("Walking SD", "Scanning files", 0, DEVICE_FS_MANIFEST_MAX);
  JsonDocument doc(&s_jsonMemory);
  doc["op_id"] = opId;
  JsonArray entries = doc["entries"].to<JsonArray>();
  int count = 0;
  const bool complete = walkInto("/", entries, count);
  // The backend must not interpret paths omitted by the bounded walk as
  // device-side deletions.
  doc["truncated"] = !complete;
  if (services::net_poll::stopRequested()) return;
  LOGI("device_fs", "manifest: %d file(s)", count);
  String body; serializeJson(doc, body);
  setStatus("Sending manifest", "Posting file list", (uint32_t)count, (uint32_t)(count > 0 ? count : 1));
  int code = httpPostJson(DEVICE_FS_MANIFEST_PATH, body);
  if (!ok2xx(code)) {
    char err[48];
    snprintf(err, sizeof(err), "Manifest failed HTTP %d", code);
    const bool retry = code <= 0 || code >= 500 || code == 401 || code == 408 || code == 429;
    // A local request has no server-side lease to recover after a timeout.
    // Reconciliation is idempotent, including when the server already accepted it.
    if (retry && (!opId || !opId[0])) {
      portENTER_CRITICAL(&s_statusMux);
      s_syncRequested = true;
      portEXIT_CRITICAL(&s_statusMux);
    }
    setStatus(retry ? "Sync retrying" : "Sync failed", err, 0, 1, retry, !retry, false);
    LOGW("device_fs", "manifest POST failed HTTP %d; retry=%d", code, retry);
  } else {
    setStatus("Syncing", "Checking changes", 1, 1);
  }
}

// Parse backendUrl into host/port/scheme/basePath for the raw push socket.
bool parseBackend(const char* url, char* host, size_t hostLen, uint16_t* port,
                  bool* https, char* base, size_t baseLen) {
  const char* p = url;
  *https = false;
  if (!strncmp(p, "https://", 8)) { *https = true; *port = 443; p += 8; }
  else if (!strncmp(p, "http://", 7)) { *port = 80; p += 7; }
  else return false;
  const char* slash = strchr(p, '/');
  const char* colon = strchr(p, ':');
  const char* hostEnd = slash ? slash : p + strlen(p);
  if (colon && colon < hostEnd) {
    *port = (uint16_t)atoi(colon + 1);
    hostEnd = colon;
  }
  size_t hlen = hostEnd - p;
  if (hlen >= hostLen) return false;
  memcpy(host, p, hlen); host[hlen] = '\0';
  if (slash) strlcpy(base, slash, baseLen); else strlcpy(base, "", baseLen);
  return true;
}

// Stream an SD file up to the server as a raw octet-stream body. Chunked reads
// under lockSpi (never held across the socket write). Aborts on stop request.
void doPush(const char* path, const char* opId) {
  String sdPath(path);
  if (isExcluded(sdPath)) { reportResult(opId, false, 0, "excluded"); return; }

  // Like UploadQueue, large file transfers need the idle Agent socket's
  // internal/DMA memory. Keep the pause until the TLS client is destroyed;
  // foreground voice cancels this poller through its existing stop protocol.
  // Do not call acquireUploadNetwork here: it would wait for this same poller.
  services::agent_coordination::TransportPause agentPause;
  if (services::net_poll::stopRequested() ||
      core::device_agent_text::hasPendingRequest() ||
      services::agent_coordination::isSessionActive() ||
      !agentPause.acquire(3000) || services::net_poll::stopRequested() ||
      core::device_agent_text::hasPendingRequest()) {
    setStatus("Upload paused", "Waiting for Agent", 0, 1, false, true, false);
    return; // Keep the server lease for retry, including failed pause admission.
  }

  const auto request = core::secrets::backendRequest();
  char host[80], base[80]; uint16_t port; bool https;
  if (!parseBackend(request.backend.url, host, sizeof(host), &port, &https, base, sizeof(base))) {
    reportResult(opId, false, 0, "bad url"); return;
  }

  hal::bus::lockSpi();
  File file = SD.open(sdPath, FILE_READ);
  size_t fileSize = file ? file.size() : 0;
  uint32_t mtime = file ? (uint32_t)file.getLastWrite() : 0;
  bool haveFile = (bool)file;
  hal::bus::unlockSpi();
  if (!haveFile) { reportResult(opId, false, 0, "missing"); return; }

  WiFiClientSecure sec; WiFiClient plain;
  if (https) { services::tls::useRootBundle(sec); sec.setHandshakeTimeout(8); }
  WiFiClient& client = https ? static_cast<WiFiClient&>(sec) : static_cast<WiFiClient&>(plain);
  if (!client.connect(host, port, 4000)) {
    LOGW("device_fs", "Upload connect failed tls=%d heap=%u largest=%u", https ? sec.lastError(nullptr, 0) : 0,
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
         (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    setStatus("Upload retrying", "Connection unavailable", 0, 1, false, true, false);
    hal::bus::lockSpi(); file.close(); hal::bus::unlockSpi();
    return;  // leave op claimed → server requeues
  }
  client.setNoDelay(true);

  String query = String(DEVICE_FS_UPLOAD_PATH) + "?path=" + urlEncode(sdPath)
               + "&mtime=" + String(mtime) + "&op_id=" + urlEncode(String(opId));
  client.printf("POST %s%s HTTP/1.1\r\n", base, query.c_str());
  client.printf("Host: %s\r\n", host);
  client.printf("X-Device-Id: %s\r\n", core::device::id());
  client.printf("Authorization: Bearer %s\r\n", request.token);
  client.print("Content-Type: application/octet-stream\r\n");
  client.printf("Content-Length: %u\r\n", (unsigned)fileSize);
  client.print("Connection: close\r\n\r\n");

  uint8_t* buf = (uint8_t*)heap_caps_malloc(kChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) { client.stop(); hal::bus::lockSpi(); file.close(); hal::bus::unlockSpi(); return; }
  size_t remaining = fileSize;
  size_t sent = 0;
  bool writeOk = true;
  int chunkCount = 0;
  while (remaining > 0 && writeOk) {
    if (services::net_poll::stopRequested() || core::device_agent_text::hasPendingRequest()) {
      writeOk = false; break;
    }
    size_t toRead = remaining < kChunk ? remaining : kChunk;
    hal::bus::lockSpi();
    size_t got = file.read(buf, toRead);
    hal::bus::unlockSpi();
    if (got == 0) { LOGW("device_fs", "Upload SD read stopped at %u/%u", (unsigned)sent, (unsigned)fileSize); break; }
    size_t offset = 0;
    while (offset < got && !services::net_poll::stopRequested() &&
           !core::device_agent_text::hasPendingRequest()) {
      const size_t wrote = client.write(buf + offset, got - offset);
      if (!wrote) { writeOk = false; break; }
      offset += wrote;
      core::watchdog::feed();
      vTaskDelay(1); // allow TCP acknowledgements to release internal send buffers
    }
    if (offset != got) writeOk = false;
    remaining -= offset;
    sent += offset;
    setStatus("Uploading", sdPath.c_str(), (uint32_t)sent, (uint32_t)fileSize);
    if ((++chunkCount & 7) == 0) { vTaskDelay(0); core::watchdog::feed(); }
  }
  free(buf);
  hal::bus::lockSpi(); file.close(); hal::bus::unlockSpi();

  if (!writeOk || remaining != 0) {
    LOGW("device_fs", "Upload interrupted at %u/%u stop=%d agent_text=%d heap=%u", (unsigned)sent, (unsigned)fileSize,
         services::net_poll::stopRequested(), core::device_agent_text::hasPendingRequest(),
         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    setStatus("Upload paused", sdPath.c_str(), (uint32_t)sent, (uint32_t)fileSize, false, true, false);
    client.stop();
    return;
  }  // requeue
  client.flush();

  // Read the status line for logging; the server marks the op done via op_id.
  uint32_t t0 = millis();
  while (client.connected() && !client.available() && millis() - t0 < 15000) {
    if (services::net_poll::stopRequested() || core::device_agent_text::hasPendingRequest()) break;
    // The bounded server wait is as long as TWDT itself. Keep this responsive
    // poll alive without extending its deadline or feeding a blocked socket.
    core::watchdog::feed();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  String status = client.readStringUntil('\n');
  client.stop();
  const int space = status.indexOf(' ');
  const int responseCode = space >= 0 ? status.substring(space + 1).toInt() : 0;
  if (responseCode == 401) core::secrets::setDeviceTokenFor(request, "");
  if (ok2xx(responseCode)) {
    setStatus("Uploaded", sdPath.c_str(), 1, 1);
  } else {
    char error[48]; snprintf(error, sizeof(error), "Upload HTTP %d", responseCode);
    setStatus("Upload failed", error, 0, 1, false, true, false);
    if (responseCode >= 400 && responseCode < 500 && responseCode != 401 && responseCode != 408 && responseCode != 429) {
      reportResult(opId, false, 0, error);
    }
    // Transient failures keep their lease and retry automatically.
  }
  LOGI("device_fs", "push %s -> HTTP %d", sdPath.c_str(), responseCode);
}

// Download a file from a signed URL (or fetch one) and write it to SD.
void doPull(const char* path, const char* url, const char* opId) {
  String sdPath(path);
  if (isExcluded(sdPath)) { reportResult(opId, false, 0, "excluded"); return; }
  if (!services::uploads::ensureInit()) {reportResult(opId,false,0,"queue unavailable");return;}
  UploadQueue::FileMutation mutation(services::uploads::queue(),path,false);
  if (!mutation.ready()) {reportResult(opId,false,0,"queued media or mutation busy");return;}

  String dl(url ? url : "");
  if (!dl.length()) {
    // No embedded URL — ask the server for a fresh signed one.
    JsonDocument doc(&s_jsonMemory);
    String p = String(DEVICE_FS_DOWNLOAD_PATH) + "?path=" + urlEncode(sdPath);
    if (ok2xx(httpGetJson(p.c_str(), doc))) dl = String(doc["url"] | "");
  }
  if (!dl.length()) { reportResult(opId, false, 0, "no url"); return; }

  const bool https = dl.startsWith("https://");
  WiFiClientSecure sec; WiFiClient plain;
  if (https) { services::tls::useRootBundle(sec); sec.setHandshakeTimeout(8); }
  WiFiClient& wc = https ? static_cast<WiFiClient&>(sec) : static_cast<WiFiClient&>(plain);

  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(15000);
  if (!http.begin(wc, dl)) { reportResult(opId, false, 0, "begin"); return; }
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.useHTTP10(true); // storage downloads need a finite byte stream, not chunk framing
  int code = http.GET();
  if (!ok2xx(code)) {
    http.end();
    char error[48]; snprintf(error, sizeof(error), "Download HTTP %d", code);
    LOGW("device_fs", "%s", error);
    setStatus("Download failed", error, 0, 1, false, true, false);
    // Transient transport/server failures remain claimed for lease recovery.
    if (code > 0 && code < 500 && code != 408 && code != 429) reportResult(opId, false, 0, error);
    return;
  }

  ensureParentDirs(sdPath);
  const int nameStart = sdPath.lastIndexOf('/') + 1;
  const String dir = nameStart > 1 ? sdPath.substring(0, nameStart) : String("/");
  const String base = sdPath.substring(nameStart);
  const String tempPath = dir + ".kami-sync-" + base;
  const String backupPath = dir + ".kami-backup-" + base;
  hal::bus::lockSpi();
  SD.remove(tempPath);
  File out = SD.open(tempPath, FILE_WRITE);
  bool haveOut = (bool)out;
  hal::bus::unlockSpi();
  if (!haveOut) { http.end(); reportResult(opId, false, 0, "open"); return; }

  WiFiClient* stream = http.getStreamPtr();
  int len = http.getSize();  // -1 if unknown (chunked)
  uint8_t* buf = (uint8_t*)heap_caps_malloc(kChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  bool okWrite = buf != nullptr;
  int chunkCount = 0;
  int totalLen = len > 0 ? len : 1;
  int received = 0;
  uint32_t idle0 = millis();
  while (okWrite && http.connected() && (len > 0 || len == -1)) {
    if (services::net_poll::stopRequested()) { okWrite = false; break; }
    size_t avail = stream->available();
    if (avail) {
      int c = stream->readBytes(buf, avail < kChunk ? avail : kChunk);
      hal::bus::lockSpi();
      size_t w = out.write(buf, c);
      hal::bus::unlockSpi();
      if ((int)w != c) { okWrite = false; break; }
      if (len > 0) len -= c;
      received += c;
      setStatus("Downloading", sdPath.c_str(), (uint32_t)received, (uint32_t)totalLen);
      idle0 = millis();
      if ((++chunkCount & 7) == 0) { vTaskDelay(0); core::watchdog::feed(); }
    } else {
      if (millis() - idle0 > 15000) { okWrite = false; break; }
      vTaskDelay(pdMS_TO_TICKS(5));
    }
  }
  if (buf) free(buf);
  uint32_t mtime = 0;
  hal::bus::lockSpi();
  out.flush();
  out.close();
  hal::bus::unlockSpi();
  http.end();

  if (okWrite && (len == 0 || len == -1)) {
    // Install only after the complete download exists. Keep the prior file as
    // a hidden same-directory backup until rename succeeds so a bad transfer
    // or SD error cannot destroy the working copy.
    hal::bus::lockSpi();
    SD.remove(backupPath);
    const bool hadOriginal = SD.exists(sdPath);
    const bool backedUp = !hadOriginal || SD.rename(sdPath, backupPath);
    const bool installed = backedUp && SD.rename(tempPath, sdPath);
    if (!installed && hadOriginal && backedUp) SD.rename(backupPath, sdPath);
    if (installed) SD.remove(backupPath);
    File re = installed ? SD.open(sdPath, FILE_READ) : File();
    if (re) { mtime = (uint32_t)re.getLastWrite(); re.close(); }
    hal::bus::unlockSpi();

    if (installed) {
      LOGI("device_fs", "pull %s ok", sdPath.c_str());
      setStatus("Downloaded", sdPath.c_str(), 1, 1);
      reportResult(opId, true, mtime);
    } else {
      hal::bus::lockSpi(); SD.remove(tempPath); hal::bus::unlockSpi();
      LOGI("device_fs", "pull %s FAILED (install)", sdPath.c_str());
      setStatus("Download failed", sdPath.c_str(), 1, 1, false, true, false);
    }
  } else {
    LOGI("device_fs", "pull %s FAILED (remaining=%d)", sdPath.c_str(), len);
    setStatus("Download failed", sdPath.c_str(), (uint32_t)received, (uint32_t)totalLen, false, true, false);
    // Leave op claimed for requeue; drop only the partial temporary file.
    hal::bus::lockSpi(); SD.remove(tempPath); hal::bus::unlockSpi();
  }
}

void doDelete(const char* path, const char* opId) {
  String sdPath(path);
  if (isExcluded(sdPath) || sdPath.length() < 2) { reportResult(opId, false, 0, "excluded"); return; }
  if (!services::uploads::ensureInit()) {reportResult(opId,false,0,"queue unavailable");return;}
  UploadQueue::FileMutation mutation(services::uploads::queue(),path,true);
  if (!mutation.ready()) {reportResult(opId,false,0,"queued media or mutation busy");return;}
  hal::bus::lockSpi();
  const bool existed = SD.exists(sdPath);
  hal::bus::unlockSpi();
  const bool ok = !existed || removeRecursive(sdPath);
  LOGI("device_fs", "delete %s -> %d", sdPath.c_str(), ok);
  setStatus(ok ? "Deleted" : "Delete failed", sdPath.c_str(), 1, 1, false, true, ok);
  reportResult(opId, ok, 0, ok ? nullptr : "delete failed");
}

void doMkdir(const char* path, const char* opId) {
  String sdPath(path);
  if (isExcluded(sdPath) || sdPath.length() < 2) { reportResult(opId, false, 0, "excluded"); return; }
  ensureParentDirs(sdPath + "/x");  // make all parents including sdPath itself
  bool exists;
  hal::bus::lockSpi();
  exists = SD.exists(sdPath);
  if (!exists) exists = SD.mkdir(sdPath);
  hal::bus::unlockSpi();
  setStatus(exists ? "Folder ready" : "Folder failed", sdPath.c_str(), 1, 1, false, true, exists);
  reportResult(opId, exists);
}

void doList(const char* path, const char* opId) {
  String dir(path && path[0] ? path : "/");
  std::vector<Child> children;
  if (!listDir(dir, children)) { reportResult(opId, false, 0, "SD directory unavailable"); return; }
  JsonDocument doc(&s_jsonMemory);
  doc["ok"] = true;
  JsonArray arr = doc["entries"].to<JsonArray>();
  for (const Child& c : children) {
    if (isExcluded(c.path)) continue;
    JsonObject e = arr.add<JsonObject>();
    e["path"] = c.path; e["is_dir"] = c.isDir; e["size"] = c.size; e["mtime"] = c.mtime;
  }
  String body; serializeJson(doc, body);
  httpPostJson(String(DEVICE_FS_OPS_PATH) + "/" + opId + "/result", body);
}

}  // namespace

bool poll() {
  if (!core::secrets::hasDeviceToken()) return false;  // not registered yet

  portENTER_CRITICAL(&s_statusMux);
  const bool requested = s_syncRequested;
  s_syncRequested = false;
  portEXIT_CRITICAL(&s_statusMux);
  if (requested) {
    LOGI("device_fs", "manual sync requested");
    doManifest(nullptr);
  }

  JsonDocument doc(&s_jsonMemory);
  int code = httpGetJson(DEVICE_FS_OPS_PATH, doc);
  if (!ok2xx(code)) return false;
  JsonArray ops = doc["ops"].as<JsonArray>();
  if (ops.isNull()) {
    finishPollStatus(0, 0);
    return true;
  }

  const uint32_t opTotal = ops.size();
  uint32_t opIndex = 0;
  for (JsonObject op : ops) {
    if (services::net_poll::stopRequested()) break;
    core::watchdog::feed();
    opIndex++;
    const char* id   = op["id"]   | "";
    const char* type = op["op"]   | "";
    const char* path = op["path"] | "";
    const char* url  = op["url"]  | "";
    LOGV("device_fs", "Executing %s op %s", type, id);
    const char* phase = "Syncing";
    if (!strcmp(type, "push")) phase = "Uploading";
    else if (!strcmp(type, "pull")) phase = "Downloading";
    else if (!strcmp(type, "delete")) phase = "Deleting";
    else if (!strcmp(type, "mkdir")) phase = "Creating folder";
    setStatus(phase, path, opIndex - 1, opTotal);
    if      (!strcmp(type, "manifest")) doManifest(id);
    else if (!strcmp(type, "push"))     doPush(path, id);
    else if (!strcmp(type, "pull"))     doPull(path, url, id);
    else if (!strcmp(type, "delete"))   doDelete(path, id);
    else if (!strcmp(type, "mkdir"))    doMkdir(path, id);
    else if (!strcmp(type, "list"))     doList(path, id);
    else reportResult(id, false, 0, "unknown op");
    core::watchdog::feed();
  }
  finishPollStatus(opTotal, doc["pending"] | 0u);
  return true;
}

void requestSync(SyncRequest request) {
  // Interactive activation preserves Settings' existing immediate wake attempt.
  // Queue-only requests never override a foreground worker pause.
  if (request == SyncRequest::Interactive) (void)services::net_poll::start();
  portENTER_CRITICAL(&s_statusMux);
  setStatusLocked("Syncing", "Starting...", 0, 1, true, false, true);
  s_syncRequested = true;
  portEXIT_CRITICAL(&s_statusMux);
  LOGI("device_fs", "sync requested");
}

void getSyncStatus(SyncStatus& out) {
  portENTER_CRITICAL(&s_statusMux);
  out = s_status;
  portEXIT_CRITICAL(&s_statusMux);
}

}  // namespace device_fs
}  // namespace services
