#include "upload_queue.h"

#include <ArduinoJson.h>
#include <SD.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include "services/tls_trust.h"
#include <esp_heap_caps.h>

#include "core/secrets.h"
#include "core/watchdog.h"
#include "hal/hal_bus.h"
#include "hal/hal_sd.h"
#include "hal/hal_tasks.h"
#include "upload_queue_policy.h"

// ---------------------------------------------------------------------------
// Concurrency model
// ---------------------------------------------------------------------------
//
// The main loop() runs on Arduino's app_main task (typically core 1). HTTP
// uploads can take seconds because:
//   * client.connect()         — DNS + TCP/TLS handshake (up to 5 s)
//   * client.write(file_chunk) — blocks while the WiFi TX queue drains
//   * client.available()       — wait for HTTP response (server transcribes
//                                + uploads to Supabase before 200, up to 30 s)
//
// Doing any of that on the loop() core froze the eye animation and dropped
// rotary detents. So we run a dedicated worker task pinned to core 0 that
// owns the network I/O. The main thread:
//   * enqueue(): adds to m_items[] under m_mutex, persists manifest,
//     notifies the worker.
// The background service alone calls tick(): it acquires network admission
// before starting a worker, or notifies an existing worker after reconnect
// or backoff expiry. Admission can wait; it does not run on the UI task.
//
// The worker:
//   * sleeps on ulTaskNotifyTake with a long timeout (poll fallback).
//   * snapshots one pending item under the mutex, releases the mutex, then
//     does the (long, blocking) HTTP transfer with NO mutex held.
//   * re-acquires the mutex to write the result back into m_items[] and
//     update the manifest.
//
// Locks are NEVER held across network I/O, so an in-flight upload doesn't
// stall main-thread enqueues.

// ---------------------------------------------------------------------------
// Tiny RAII lock guard
// ---------------------------------------------------------------------------
namespace {
struct MutexGuard {
    SemaphoreHandle_t m;
    bool taken = false;
    MutexGuard(SemaphoreHandle_t mtx, TickType_t to = portMAX_DELAY) : m(mtx) {
        if (m && xSemaphoreTake(m, to) == pdTRUE) taken = true;
    }
    ~MutexGuard() { if (taken && m) xSemaphoreGive(m); }
};
}  // namespace

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------

bool UploadQueue::init() {
    if (m_initialized) return true;
    if (!hal::sd::mounted()) {
        Serial.println("[queue] SD unavailable; upload queue disabled");
        return false;
    }

    if (!m_items) {
        m_items = static_cast<QueueItem*>(heap_caps_calloc(
            UPLOAD_QUEUE_MAX_ITEMS, sizeof(QueueItem),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!m_items) {
            Serial.printf("[queue] PSRAM metadata allocation failed: bytes=%u "
                          "psramFree=%u psramLargest=%u\n",
                          (unsigned)(UPLOAD_QUEUE_MAX_ITEMS * sizeof(QueueItem)),
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
            return false;
        }
    }

    m_lastStatus[0] = '\0';
    m_count = 0;

    if (!m_mutex) m_mutex = xSemaphoreCreateMutex();
    if (!m_mutex) {
        Serial.println("[queue] Failed to create mutex");
        heap_caps_free(m_items);
        m_items = nullptr;
        return false;
    }

    {
        MutexGuard g(m_mutex);

        // Create queue directory
        if (!SD.exists(UPLOAD_QUEUE_DIR)) {
            SD.mkdir(UPLOAD_QUEUE_DIR);
        }

        // Recover an interrupted replacement before accepting new queue work.
        if (!loadManifestUnlocked()) {
            Serial.println("[queue] Manifest recovery failed; preserving files and disabling queue");
            return false;
        }

    }

    m_initialized = true;
    Serial.printf("[queue] Initialized: %d items, nextImg=%lu, nextAud=%lu, "
                  "metadata=%uB PSRAM\n",
                  m_count, (unsigned long)m_nextImgId, (unsigned long)m_nextAudId,
                  (unsigned)(UPLOAD_QUEUE_MAX_ITEMS * sizeof(QueueItem)));

    return true;
}

// ---------------------------------------------------------------------------
// Manifest persistence (JSON on SD) — caller MUST hold m_mutex.
// ---------------------------------------------------------------------------

bool UploadQueue::loadManifestUnlocked() {
    const char* candidates[] = {UPLOAD_QUEUE_MANIFEST,
                               UPLOAD_QUEUE_MANIFEST ".tmp",
                               UPLOAD_QUEUE_MANIFEST ".bak"};
    bool found = false;
    for (const char* path : candidates) {
        if (!SD.exists(path)) continue;
        found = true;
        if (!loadManifestFileUnlocked(path)) continue;
        if (strcmp(path, UPLOAD_QUEUE_MANIFEST) != 0) {
            // Promote the recovered copy before another save can reuse .tmp/.bak.
            if (SD.exists(UPLOAD_QUEUE_MANIFEST) && !SD.remove(UPLOAD_QUEUE_MANIFEST)) return false;
            if (!SD.rename(path, UPLOAD_QUEUE_MANIFEST)) return false;
            Serial.println("[queue] Recovered interrupted manifest replacement");
        }
        return true;
    }
    return !found; // A genuinely new SD card has no prior queue state.
}

bool UploadQueue::loadManifestFileUnlocked(const char* path) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();

    if (err) {
        Serial.printf("[queue] JSON parse error: %s\n", err.c_str());
        return false;
    }

    // Reject syntactically valid but incomplete files before publishing state.
    if (!doc["items"].is<JsonArray>() ||
        !doc["nextImgId"].is<uint32_t>() || !doc["nextAudId"].is<uint32_t>() ||
        doc["nextImgId"].as<uint32_t>() == 0 || doc["nextAudId"].as<uint32_t>() == 0 ||
        doc["items"].size() > UPLOAD_QUEUE_MAX_ITEMS) return false;
    for (JsonObject item : doc["items"].as<JsonArray>()) {
        if (!item["id"].is<const char*>() || !item["path"].is<const char*>() ||
            !item["type"].is<const char*>() || !item["mime"].is<const char*>()) return false;
    }

    m_nextImgId = doc["nextImgId"] | 1;
    m_nextAudId = doc["nextAudId"] | 1;

    JsonArray items = doc["items"].as<JsonArray>();
    m_count = 0;
    for (JsonObject obj : items) {
        if (m_count >= UPLOAD_QUEUE_MAX_ITEMS) break;

        QueueItem& it = m_items[m_count];
        strlcpy(it.id,       obj["id"] | "",    sizeof(it.id));
        strlcpy(it.type,     obj["type"] | "",   sizeof(it.type));
        strlcpy(it.filePath, obj["path"] | "",   sizeof(it.filePath));
        strlcpy(it.mimeType, obj["mime"] | "",   sizeof(it.mimeType));
        strlcpy(it.fileName, obj["name"] | "",   sizeof(it.fileName));
        it.createdAt     = obj["created"] | (uint32_t)0;
        it.attempts      = obj["attempts"] | (uint8_t)0;
        // millis()-based state cannot survive reboot. Every boot starts a new
        // turn, but the lifetime attempt count and queue order remain durable.
        it.lastAttemptAt = 0;
        it.nextEligibleAt = 0;
        it.attemptsThisTurn = 0;
        it.status        = (QueueItemStatus)(obj["status"] | 0);
        it.deleteAfterUpload = obj["deleteAfterUpload"] | true;
        strlcpy(it.lastError, obj["error"] | "", sizeof(it.lastError));

        // Reset items stuck in UPLOADING state (interrupted by reboot OR by
        // the worker dying mid-upload). They'll be re-attempted on next tick.
        if (it.status == QSTATUS_UPLOADING) {
            it.status = QSTATUS_PENDING;
        }

        // Drop FAILED items left over from earlier firmware versions — the new code path
        // never persists FAILED (it deletes immediately on permanent failure / max retries),
        // so anything in this state is stale. Keeping them around just wastes queue slots.
        if (it.status == QSTATUS_FAILED) {
            Serial.printf("[queue] Boot purge: dropping stale FAILED %s (err=%s)\n", it.id, it.lastError);
            continue;
        }

        // Verify file still exists on SD
        if (it.status == QSTATUS_PENDING && !SD.exists(it.filePath)) {
            Serial.printf("[queue] Dropping %s - file missing\n", it.id);
            continue;
        }

        m_count++;
    }

    return true;
}

bool UploadQueue::saveManifestUnlocked() {
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    if (m_testMode) return true; // Isolated control tests never touch SD.
#endif
    JsonDocument doc;
    doc["schemaVersion"] = 2;
    doc["nextImgId"] = m_nextImgId;
    doc["nextAudId"] = m_nextAudId;

    JsonArray items = doc["items"].to<JsonArray>();
    for (int i = 0; i < m_count; i++) {
        QueueItem& it = m_items[i];
        JsonObject obj = items.add<JsonObject>();
        obj["id"]          = it.id;
        obj["type"]        = it.type;
        obj["path"]        = it.filePath;
        obj["mime"]        = it.mimeType;
        obj["name"]        = it.fileName;
        obj["created"]     = it.createdAt;
        obj["attempts"]    = it.attempts;
        obj["status"]      = (int)it.status;
        obj["deleteAfterUpload"] = it.deleteAfterUpload;
        obj["error"]       = it.lastError;
    }

    if (doc.overflowed()) return false;
    const char* temp = UPLOAD_QUEUE_MANIFEST ".tmp";
    const char* backup = UPLOAD_QUEUE_MANIFEST ".bak";
    // FILE_WRITE appends on this platform: remove only the disposable scratch file.
    if (SD.exists(temp) && !SD.remove(temp)) return false;
    File f = SD.open(temp, FILE_WRITE);
    if (!f) return false;
    const size_t expected = measureJson(doc);
    const size_t written = serializeJson(doc, f);
    f.flush();
    const size_t actual = f.size();
    f.close();
    if (written != expected || actual != expected) return false;

    // Same-directory replacement follows device_fs's temp/backup contract.
    // Keep the previous valid manifest until the next replacement. Boot handles
    // interruption between either rename; never unlink the only current copy.
    if (SD.exists(UPLOAD_QUEUE_MANIFEST)) {
        if (SD.exists(backup) && !SD.remove(backup)) return false;
        if (!SD.rename(UPLOAD_QUEUE_MANIFEST, backup)) return false;
    }
    if (!SD.rename(temp, UPLOAD_QUEUE_MANIFEST)) {
        // Leave both candidates intact for boot recovery or the next save.
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Generate unique file path
// ---------------------------------------------------------------------------

uint32_t UploadQueue::generatePath(const char* prefix, const char* ext,
                                   char* outPath, size_t outPathSize) {
    MutexGuard g(m_mutex);
    uint32_t seq;
    if (strncmp(prefix, "IMG", 3) == 0) {
        seq = m_nextImgId++;
    } else {
        seq = m_nextAudId++;
    }

    snprintf(outPath, outPathSize, "%s/%s_%05lu%s",
             UPLOAD_QUEUE_DIR, prefix, (unsigned long)seq, ext);

    if (!saveManifestUnlocked()) {
        outPath[0] = '\0';
        return 0;
    }
    return seq;
}

// ---------------------------------------------------------------------------
// Enqueue
// ---------------------------------------------------------------------------

bool UploadQueue::enqueue(const char* type, const char* filePath,
                          const char* mimeType, bool deleteAfterUpload) {
    {
        MutexGuard g(m_mutex);
        if (m_count >= UPLOAD_QUEUE_MAX_ITEMS) {
            // Don't drop the new item — evict the oldest expendable one so the user's latest
            // photo / recording is the one that survives. (Old behaviour returned false here, which
            // left the photo on SD with no upload path and the queue stuck full forever.)
            if (!makeRoomForOneUnlocked()) {
                Serial.println("[queue] Queue full and nothing evictable!");
                return false;
            }
        }

        // Verify file exists (cheap I/O outside the worker's network path).
        if (!SD.exists(filePath)) {
            Serial.printf("[queue] File not found: %s\n", filePath);
            return false;
        }

        QueueItem& it = m_items[m_count];
        memset(&it, 0, sizeof(QueueItem));

        const char* slash = strrchr(filePath, '/');
        const char* fname = slash ? (slash + 1) : filePath;

        strlcpy(it.id,       fname,    sizeof(it.id));
        strlcpy(it.type,     type,     sizeof(it.type));
        strlcpy(it.filePath, filePath, sizeof(it.filePath));
        strlcpy(it.mimeType, mimeType, sizeof(it.mimeType));
        strlcpy(it.fileName, fname,    sizeof(it.fileName));
        it.createdAt     = millis();
        it.attempts      = 0;
        it.lastAttemptAt = 0;
        it.nextEligibleAt = 0;
        it.attemptsThisTurn = 0;
        it.status        = QSTATUS_PENDING;
        it.deleteAfterUpload = deleteAfterUpload;
        it.lastError[0]  = '\0';

        m_count++;
        saveManifestUnlocked();

        Serial.printf("[queue] Enqueued %s (%s) [%d in queue]\n",
                      it.id, type, m_count);
    }

    // Wake an existing worker. A stopped worker is launched by the service
    // tick, which can safely wait for competing network users to quiesce.
    notifyWorker();
    return true;
}

// ---------------------------------------------------------------------------
// Background service tick: may wait for network ownership before worker start.
// ---------------------------------------------------------------------------

void UploadQueue::tick() {
    if (!m_initialized) return;
    // If the worker exists, tick is a no-op except to nudge it. The worker
    // also wakes itself periodically, so a missed nudge is not catastrophic.
    if (workerRunning()) {
        if (!m_uploadPaused && WiFi.status() == WL_CONNECTED) notifyWorker();
        return;
    }
    if (m_uploadPaused || WiFi.status() != WL_CONNECTED || !hasRunnableItem()) return;
    startWorker();
}

// ---------------------------------------------------------------------------
// Worker task
// ---------------------------------------------------------------------------

void UploadQueue::workerTaskTrampoline(void* arg) {
    static_cast<UploadQueue*>(arg)->workerLoop();
}

void UploadQueue::workerLoop() {
    // A task may run before spawn returns its handle to the creator.
    for (;;) {
        portENTER_CRITICAL(&m_workerMux);
        const bool starting = m_workerPhase == WorkerPhase::Starting;
        portEXIT_CRITICAL(&m_workerMux);
        if (!starting) break;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    // Long-poll wait so the worker drops to ~0% CPU when there's nothing to
    // do. Wakes early on enqueue() / tick() via xTaskNotifyGive.
    static constexpr TickType_t kIdleWaitMs = 5000;

    core::watchdog::subscribe();  // freeze detection for the upload worker

    while (!m_workerStop) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kIdleWaitMs));
        // Idle long-poll wakes at least every kIdleWaitMs (< TWDT timeout), so
        // feeding here covers the idle case; doUpload() feeds inside its I/O loops.
        core::watchdog::feed();
        if (m_workerStop) break;

        if (pendingCount() <= 0) break;
        if (WiFi.status() != WL_CONNECTED) break;

        // Drain as many pending items as possible per wake cycle. Each call
        // to processOnePending() does at most one upload (and snapshots the
        // backoff state under the mutex), so this loop terminates as soon as
        // there's nothing eligible to upload.
        while (!m_workerStop && WiFi.status() == WL_CONNECTED && !m_uploadPaused && processOnePending()) {
            // Yield briefly so other core-0 work (Wi-Fi stack, logging) gets
            // a slot between back-to-back uploads.
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        if (pendingCount() <= 0) break;
        // Remaining items are in backoff or resources were preempted. Drop the
        // task and network lease; the service tick will recreate both when the
        // next retry becomes eligible.
        break;
    }
    const uint32_t minStackFree = uxTaskGetStackHighWaterMark(nullptr);
    core::watchdog::unsubscribe();  // MUST unsubscribe before deleting the task
    Serial.printf("[queue] Upload worker exiting: minStackFree=%uB\n",
                  (unsigned)minStackFree);
    retireWorker(); // Final owner-state access; only RTOS deletion follows.
    hal::tasks::deleteCurrent(hal::tasks::Id::UploadQ);
}

// ---------------------------------------------------------------------------
// Process one pending item. Returns true if it performed work that may allow
// another item to run immediately. Returns false when all work is waiting.
// Network I/O happens with NO mutex held.
// ---------------------------------------------------------------------------

bool UploadQueue::processOnePending() {
    QueueItem snapshot;
    bool have = false;

    // 1) Snapshot the next eligible item under the mutex.
    {
        MutexGuard g(m_mutex);
        if (m_uploadPaused) return false;
        const uint32_t now = millis();
        if (forcedIndexUnlocked() < 0 && upload_queue_policy::deadlinePending(now, m_nextNetworkRetryAt)) return false;
        m_nextNetworkRetryAt = 0;

        const int itemIdx = findRunnableIndexUnlocked(now);
        if (itemIdx < 0) return false;
        QueueItem* item = &m_items[itemIdx];
        if (strcmp(item->id, m_forcedItemId) == 0) m_forcedItemId[0] = 0;

        item->status        = QSTATUS_UPLOADING;
        item->lastAttemptAt = now;
        item->nextEligibleAt = 0;
        snprintf(m_lastStatus, sizeof(m_lastStatus), "Uploading %s...", item->id);
        snapshot = *item;
        have = true;
        saveManifestUnlocked();
    }

    if (!have) return false;

    Serial.printf("[queue] Uploading %s (lifetime=%u, turn=%u)...\n",
                  snapshot.id, (unsigned)(snapshot.attempts + 1),
                  (unsigned)(snapshot.attemptsThisTurn + 1));

    // 2) Long blocking network I/O — NO mutex held. startWorker() acquired
    // exclusive use of the device's constrained TLS resources before spawn.
    const UploadResult result = doUpload(snapshot);

    // 3) Apply result back to the live item under the mutex.
    {
        MutexGuard g(m_mutex);
        const uint32_t now = millis();

        // Find the live item by id (index could have shifted if main thread
        // enqueued another item while the upload was running).
        QueueItem* live = nullptr;
        int liveIdx = -1;
        for (int i = 0; i < m_count; i++) {
            if (strcmp(m_items[i].id, snapshot.id) == 0) {
                live = &m_items[i];
                liveIdx = i;
                break;
            }
        }
        if (!live) { m_resetAfterFlight=false; return true; } // cancelled mid-upload

        const char* kinds[] = {"http", "transfer", "file", "prerequisite", "interrupted"};
        const bool succeeded = result.kind == UploadResultKind::HttpResponse &&
                               result.httpCode >= 200 && result.httpCode < 300;
        const bool charged = result.attemptStarted &&
            (result.kind==UploadResultKind::HttpResponse || result.kind==UploadResultKind::RetryableFailure) &&
            !(m_resetAfterFlight && !succeeded);
        Serial.printf("[queue-result] item=%s kind=%s http=%d requestStarted=%u charged=%u "
                      "lifetimeBefore=%u turnBefore=%u elapsedMs=%lu reason=%s\n",
                      snapshot.id, kinds[(unsigned)result.kind], result.httpCode, result.attemptStarted, charged,
                      live->attempts, live->attemptsThisTurn,
                      (unsigned long)(now-snapshot.lastAttemptAt), snapshot.lastError);
        if (applyResetAfterFlightUnlocked(*live, succeeded)) {
            saveManifestUnlocked();
            return true;
        }
        strlcpy(live->lastError, snapshot.lastError, sizeof(live->lastError));

        if (result.kind == UploadResultKind::Aborted) {
            live->status = QSTATUS_PENDING;
            live->nextEligibleAt = 0;
            snprintf(m_lastStatus, sizeof(m_lastStatus), "%s paused", live->id);
            saveManifestUnlocked();
            return false;
        }

        if (result.kind == UploadResultKind::GlobalDeferral) {
            live->status = QSTATUS_PENDING;
            m_nextNetworkRetryAt = now + result.deferMs;
            snprintf(m_lastStatus, sizeof(m_lastStatus), "Queue retry in %lus",
                     (unsigned long)(result.deferMs / 1000u));
            Serial.printf("[queue] Global pre-request deferral for %lu ms: %s\n",
                          (unsigned long)result.deferMs, live->lastError);
            saveManifestUnlocked();
            return false;
        }

        if (result.kind == UploadResultKind::PermanentItemFailure) {
            Serial.printf("[queue] %s terminal preflight failure; preserving %s (%s)\n",
                          live->id, live->filePath, live->lastError);
            snprintf(m_lastStatus, sizeof(m_lastStatus), "%s removed: %s",
                     live->id, live->lastError);
            removeItemAtUnlocked(liveIdx);
            saveManifestUnlocked();
            return true;
        }

        if (result.attemptStarted) {
            live->attempts++;
            live->attemptsThisTurn++;
        }

        if (result.kind == UploadResultKind::HttpResponse &&
            result.httpCode >= 200 && result.httpCode < 300) {
            Serial.printf("[queue] %s uploaded OK (%d)\n", live->id, result.httpCode);
            snprintf(m_lastStatus, sizeof(m_lastStatus), "%s uploaded OK", live->id);
            deleteFileIfOwnedUnlocked(*live);
            removeItemAtUnlocked(liveIdx);
            saveManifestUnlocked();
            return true;
        }

        if (upload_queue_policy::lifetimeExhausted(live->attempts)) {
            Serial.printf("[queue] %s exhausted %u lifetime attempts; preserving %s\n",
                          live->id, (unsigned)live->attempts, live->filePath);
            snprintf(m_lastStatus, sizeof(m_lastStatus), "%s removed: max attempts", live->id);
            removeItemAtUnlocked(liveIdx);
            saveManifestUnlocked();
            return true;
        }

        if (result.kind == UploadResultKind::HttpResponse &&
            (result.httpCode == 401 || result.httpCode == 403)) {
            live->status = QSTATUS_PENDING;
            if (upload_queue_policy::turnExhausted(live->attemptsThisTurn)) {
                const uint8_t completedTurns =
                    live->attempts / upload_queue_policy::kAttemptsPerTurn;
                live->attemptsThisTurn = 0;
                live->nextEligibleAt = now + jitteredDelayMs(
                    upload_queue_policy::turnCooldownMs(completedTurns));
                rotateItemToBackUnlocked(liveIdx);
            }
            m_nextNetworkRetryAt = now + 60000u;
            snprintf(m_lastStatus, sizeof(m_lastStatus), "Authentication required");
            Serial.printf("[queue] Authentication failure HTTP %d; queue paused 60s\n",
                          result.httpCode);
            saveManifestUnlocked();
            return false;
        }

        if (result.kind == UploadResultKind::HttpResponse && !isRetryable(result.httpCode)) {
            Serial.printf("[queue] %s permanent HTTP %d; preserving %s\n",
                          live->id, result.httpCode, live->filePath);
            snprintf(m_lastStatus, sizeof(m_lastStatus), "%s removed: HTTP %d",
                     live->id, result.httpCode);
            removeItemAtUnlocked(liveIdx);
            saveManifestUnlocked();
            return true;
        }

        live->status = QSTATUS_PENDING;
        if (upload_queue_policy::turnExhausted(live->attemptsThisTurn)) {
            const uint8_t completedTurns =
                live->attempts / upload_queue_policy::kAttemptsPerTurn;
            const uint32_t cooldownMs = jitteredDelayMs(
                upload_queue_policy::turnCooldownMs(completedTurns));
            live->attemptsThisTurn = 0;
            live->nextEligibleAt = now + cooldownMs;
            Serial.printf("[queue] %s completed turn %u; rotating with %lu ms cooldown\n",
                          live->id, (unsigned)completedTurns,
                          (unsigned long)cooldownMs);
            snprintf(m_lastStatus, sizeof(m_lastStatus), "%s rotated; cooldown %lum",
                     live->id, (unsigned long)(cooldownMs / 60000u));
            rotateItemToBackUnlocked(liveIdx);
        } else {
            const uint32_t retryMs = jitteredDelayMs(
                upload_queue_policy::retryDelayMs(live->attemptsThisTurn));
            live->nextEligibleAt = now + retryMs;
            Serial.printf("[queue] %s retains turn; retry %u/4 in %lu ms\n",
                          live->id, (unsigned)(live->attemptsThisTurn + 1),
                          (unsigned long)retryMs);
            snprintf(m_lastStatus, sizeof(m_lastStatus), "%s retry in %lus",
                     live->id, (unsigned long)(retryMs / 1000u));
        }
        saveManifestUnlocked();
    }
    return true;
}

// ---------------------------------------------------------------------------
// Upload one item via multipart/form-data POST to /api/chat. Runs on the
// worker task; safe to take seconds because nothing else on core 1 is
// blocked by it. Owns fresh URL/key inputs for this attempt so a Settings
// change takes effect on the next upload, not halfway through request setup.
// ---------------------------------------------------------------------------

UploadQueue::UploadResult UploadQueue::doUpload(QueueItem& item) {
    const auto config = core::secrets::backendConfig();
    const char* liveUrl = config.url;
    const char* liveKey = config.apiKey;

    if (!liveUrl || !liveUrl[0] || !liveKey || !liveKey[0]) {
        strlcpy(item.lastError, "Backend credentials missing", sizeof(item.lastError));
        return {UploadResultKind::GlobalDeferral, -1, false, 60000u};
    }

    char host[64] = {0};
    char basePath[64] = {0};
    uint16_t port = 80;
    if (!parseUrl(liveUrl, host, sizeof(host), &port, basePath, sizeof(basePath))) {
        strlcpy(item.lastError, "Bad URL", sizeof(item.lastError));
        return {UploadResultKind::GlobalDeferral, -1, false, 60000u};
    }

    File file = SD.open(item.filePath, FILE_READ);
    if (!file) {
        strlcpy(item.lastError, "File missing", sizeof(item.lastError));
        return {UploadResultKind::PermanentItemFailure, 0, false, 0};
    }
    size_t fileSize = file.size();

    const char* boundary = "----OrigamiBoundary7MA4YWxk";

    char* preamble = (char*)malloc(512);
    if (!preamble) {
        file.close();
        strlcpy(item.lastError, "Alloc fail", sizeof(item.lastError));
        return {UploadResultKind::GlobalDeferral, -1, false, 5000u};
    }

    int preambleLen = snprintf(preamble, 512,
        "--%s\r\n"
        "Content-Disposition: form-data; name=\"media\"; filename=\"%s\"\r\n"
        "Content-Type: %s\r\n\r\n",
        boundary, item.fileName, item.mimeType);

    // Postamble: platform + sessionId + optional upload intent fields.
    char postamble[384];
    int postambleLen;
    if (strcmp(item.type, "audio") == 0) {
        postambleLen = snprintf(postamble, sizeof(postamble),
            "\r\n--%s\r\n"
            "Content-Disposition: form-data; name=\"platform\"\r\n\r\n"
            "device\r\n"
            "--%s\r\n"
            "Content-Disposition: form-data; name=\"sessionId\"\r\n\r\n"
            "origami-device\r\n"
            "--%s\r\n"
            "Content-Disposition: form-data; name=\"agentMode\"\r\n\r\n"
            "timeline_only\r\n"
            "--%s--\r\n",
            boundary, boundary, boundary, boundary);
    } else {
        postambleLen = snprintf(postamble, sizeof(postamble),
            "\r\n--%s\r\n"
            "Content-Disposition: form-data; name=\"platform\"\r\n\r\n"
            "device\r\n"
            "--%s\r\n"
            "Content-Disposition: form-data; name=\"sessionId\"\r\n\r\n"
            "origami-device\r\n"
            "--%s--\r\n",
            boundary, boundary, boundary);
    }

    size_t contentLength = (size_t)preambleLen + fileSize + (size_t)postambleLen;

    // HTTPS verifies against the shared CA store. An explicit HTTP endpoint
    // retains its plain client; failed TLS never downgrades to that client.
    const bool isHttps = (strncmp(liveUrl, "https://", 8) == 0);
    WiFiClientSecure secClient;
    WiFiClient       plainClient;
    if (isHttps) services::tls::useRootBundle(secClient);
    WiFiClient& client = isHttps ? (WiFiClient&)secClient : plainClient;

    static constexpr int32_t kConnectTimeoutMs = 2500;
    // Start a fresh watchdog interval at this bounded blocking boundary:
    // DNS <= 2.5 s, TCP <= 2.5 s, shared TLS handshake <= 8 s.
    core::watchdog::feed();
    if (!client.connect(host, port, kConnectTimeoutMs)) {
        file.close();
        free(preamble);
        strlcpy(item.lastError, "Connect failed", sizeof(item.lastError));
        Serial.printf("[queue] Connect to %s:%d failed\n", host, port);
        return {UploadResultKind::GlobalDeferral, -1, false, 5000u};
    }
    core::watchdog::feed();
    client.setNoDelay(true);

    // Allocate every transfer buffer before sending the request line. A local
    // allocation failure is therefore a pre-request deferral and cannot consume
    // one of the item's 20 lifetime attempts.
    // TLS uses PSRAM but hardware AES still needs small DMA-capable internal
    // bounce buffers. A 4 KB internal SD buffer plus a large TLS record exhausted
    // that pool after normal radio churn. Keep staging in PSRAM and bound records.
    static constexpr size_t kUploadChunkBytes = 1024;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(kUploadChunkBytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        client.stop();
        file.close();
        free(preamble);
        strlcpy(item.lastError, "Buf alloc fail", sizeof(item.lastError));
        return {UploadResultKind::GlobalDeferral, -1, false, 5000u};
    }

    char endpoint[128];
    snprintf(endpoint, sizeof(endpoint), "%s/api/chat", basePath);

    // Stream writes may make partial progress. Validate every byte of the
    // envelope as well as the media; never wait for a response to a short body.
    auto writeAll = [&](const uint8_t* data, size_t size) {
        size_t offset = 0;
        const uint32_t started = millis();
        while (offset < size) {
            if (m_workerStop || m_uploadPaused || millis() - started >= 10000u) return false;
            const size_t chunk = (size - offset < kUploadChunkBytes) ? size - offset : kUploadChunkBytes;
            const size_t written = client.write(data + offset, chunk);
            if (!written || written > chunk) return false;
            offset += written;
            core::watchdog::feed();
        }
        return true;
    };
    char headers[640];
    const int headerLen = snprintf(headers, sizeof(headers),
        "POST %s HTTP/1.1\r\nHost: %s\r\nX-Api-Key: %s\r\n"
        "Accept-Language: en\r\nContent-Type: multipart/form-data; boundary=%s\r\n"
        "Content-Length: %u\r\nConnection: close\r\n\r\n",
        endpoint, host, liveKey, boundary, (unsigned)contentLength);
    bool envelopeOk = headerLen > 0 && headerLen < (int)sizeof(headers) &&
        preambleLen > 0 && preambleLen < 512 &&
        postambleLen > 0 && postambleLen < (int)sizeof(postamble) &&
        writeAll((const uint8_t*)headers, headerLen) &&
        writeAll((const uint8_t*)preamble, preambleLen);
    free(preamble);
    preamble = nullptr;
    if (!envelopeOk) {
        client.stop(); free(buf); file.close();
        strlcpy(item.lastError, "Request write failed", sizeof(item.lastError));
        return {m_workerStop || m_uploadPaused ? UploadResultKind::Aborted :
            UploadResultKind::RetryableFailure, -1, true, 0};
    }

    // The bounded read buffer lives in PSRAM rather than internal RAM or stack.
    size_t remaining = fileSize;
    bool writeOk = true;
    int chunkCount = 0;
    while (remaining > 0 && writeOk) {
        if (m_workerStop || m_uploadPaused) {  // voice/music wants the radio — yield now
            client.stop();
            free(buf);
            file.close();
            strlcpy(item.lastError, "Aborted for voice", sizeof(item.lastError));
            return {UploadResultKind::Aborted, -1, false, 0};
        }
        size_t toRead = (remaining < kUploadChunkBytes) ? remaining : kUploadChunkBytes;
        // Hold the SPI mutex for the SD read so the display flush on Core 1
        // (which also holds lockSpi) cannot interleave with the SD transaction.
        // Without this, file.read() uses SPI.beginTransaction() directly,
        // bypassing the hal mutex.  Eye animation pushSprite() calls also use
        // SPI.beginTransaction() directly; worst-case stall is one SD sector
        // read (~200 µs normal, ~100 ms SD wear-leveling).  Holding lockSpi
        // for one bounded chunk keeps SD/display transactions serialized.
        hal::bus::lockSpi();
        size_t bytesRead = file.read(buf, toRead);
        hal::bus::unlockSpi();
        if (bytesRead == 0 || bytesRead > toRead) { writeOk = false; break; }
        writeOk = writeAll(buf, bytesRead); // Network I/O holds no SPI lock.
        remaining -= bytesRead;
        // Each write can consume its socket timeout. Eight successful but slow
        // chunks must not accumulate past the watchdog between progress feeds.
        core::watchdog::feed();
        if ((++chunkCount & 7) == 0) vTaskDelay(0);
    }
    free(buf);
    file.close();

    if (!writeOk || remaining != 0) {
        client.stop();
        strlcpy(item.lastError, m_workerStop || m_uploadPaused ? "Aborted for voice" : "Write failed", sizeof(item.lastError));
        return {m_workerStop || m_uploadPaused ? UploadResultKind::Aborted :
            UploadResultKind::RetryableFailure, -1, true, 0};
    }

    if (!writeAll((const uint8_t*)postamble, postambleLen)) {
        client.stop();
        strlcpy(item.lastError, "Multipart tail write failed", sizeof(item.lastError));
        return {m_workerStop || m_uploadPaused ? UploadResultKind::Aborted :
            UploadResultKind::RetryableFailure, -1, true, 0};
    }
    client.flush();

    // Server processes file (Supabase upload, transcription) before sending
    // HTTP 200, so allow plenty of time. We're on a worker task — this is
    // not stalling the UI loop.
    uint32_t startMs = millis();
    static constexpr uint32_t kRespWaitMs = 30000;
    while (client.connected() && !client.available()) {
        if (m_workerStop || m_uploadPaused) {  // voice/music wants the radio — yield now
            client.stop();
            strlcpy(item.lastError, "Aborted for voice", sizeof(item.lastError));
            return {UploadResultKind::Aborted, -1, false, 0};
        }
        if (millis() - startMs > kRespWaitMs) {
            client.stop();
            strlcpy(item.lastError, "Timeout", sizeof(item.lastError));
            return {UploadResultKind::RetryableFailure, -1, true, 0};
        }
        vTaskDelay(pdMS_TO_TICKS(2));
        core::watchdog::feed();  // server can take up to kRespWaitMs (30s) > TWDT timeout
    }

    // TCP/TLS can fragment even the status line one byte at a time. Wait for
    // its terminator before parsing; "HTTP/1.1 20" is not an HTTP 20 response.
    char statusBuf[96] = {};
    size_t idx = 0;
    bool complete = false;
    while (millis() - startMs < kRespWaitMs && idx < sizeof(statusBuf) - 1) {
        if (m_workerStop || m_uploadPaused) {
            client.stop();
            strlcpy(item.lastError, "Aborted for voice", sizeof(item.lastError));
            return {UploadResultKind::Aborted, -1, false, 0};
        }
        if (client.available()) {
            const char c = client.read();
            if (c == '\n') { complete = true; break; }
            statusBuf[idx++] = c;
        } else {
            if (!client.connected()) break;
            vTaskDelay(pdMS_TO_TICKS(2));
        }
        core::watchdog::feed();
    }
    int httpCode = -1;
    if (complete && idx >= 13 &&
        (!strncmp(statusBuf, "HTTP/1.1 ", 9) || !strncmp(statusBuf, "HTTP/1.0 ", 9)) &&
        statusBuf[9] >= '2' && statusBuf[9] <= '5' &&
        statusBuf[10] >= '0' && statusBuf[10] <= '9' &&
        statusBuf[11] >= '0' && statusBuf[11] <= '9' &&
        (statusBuf[12] == ' ' || statusBuf[12] == '\r')) {
        httpCode = (statusBuf[9]-'0')*100 + (statusBuf[10]-'0')*10 + statusBuf[11]-'0';
    }

    client.stop();
    Serial.printf("[queue] Upload response: %d\n", httpCode);
    if (httpCode > 0) {
        snprintf(item.lastError, sizeof(item.lastError), "HTTP %d", httpCode);
        return {UploadResultKind::HttpResponse, httpCode, true, 0};
    }
    strlcpy(item.lastError, "No HTTP response", sizeof(item.lastError));
    return {UploadResultKind::RetryableFailure, -1, true, 0};
}

// ---------------------------------------------------------------------------
// Helpers (caller must hold m_mutex for the unlocked variants).
// ---------------------------------------------------------------------------

int UploadQueue::findRunnableIndexUnlocked(uint32_t now) const {
    const int forced = forcedIndexUnlocked();
    if (forced >= 0) return forced;
    return upload_queue_policy::findRunnableIndex(
        m_items, m_count, now, QSTATUS_PENDING);
}

bool UploadQueue::hasRunnableItem() {
    MutexGuard g(m_mutex);
    if (!g.taken || m_uploadPaused || !m_items) return false;
    const uint32_t now = millis();
    if (forcedIndexUnlocked() < 0 && upload_queue_policy::deadlinePending(now, m_nextNetworkRetryAt)) return false;
    m_nextNetworkRetryAt = 0;
    return findRunnableIndexUnlocked(now) >= 0;
}

void UploadQueue::removeItemAtUnlocked(int index) {
    if (index < 0 || index >= m_count) return;
    if(!strcmp(m_items[index].id,m_forcedItemId)) m_forcedItemId[0]=0;
    if(m_items[index].status==QSTATUS_UPLOADING) m_resetAfterFlight=false;
    for (int i = index; i < m_count - 1; i++) {
        m_items[i] = m_items[i + 1];
    }
    m_count--;
}

void UploadQueue::rotateItemToBackUnlocked(int index) {
    upload_queue_policy::rotateToBack(m_items, m_count, index);
}

void UploadQueue::deleteFileIfOwnedUnlocked(const QueueItem& item) {
    if (item.deleteAfterUpload && item.filePath[0]) {
        SD.remove(item.filePath);
    }
}

bool UploadQueue::makeRoomForOneUnlocked() {
    if (m_count < UPLOAD_QUEUE_MAX_ITEMS) return true;

    // Pass 1: drop a FAILED entry (oldest first).
    for (int i = 0; i < m_count; i++) {
        if (m_items[i].status == QSTATUS_FAILED) {
            Serial.printf("[queue] Evict FAILED %s to make room (err=%s)\n",
                          m_items[i].id, m_items[i].lastError);
            removeItemAtUnlocked(i);
            saveManifestUnlocked();
            return true;
        }
    }
    // Pass 2: drop a PENDING entry that already burnt through its retry budget.
    for (int i = 0; i < m_count; i++) {
        if (m_items[i].status == QSTATUS_PENDING &&
            upload_queue_policy::lifetimeExhausted(m_items[i].attempts)) {
            Serial.printf("[queue] Evict exhausted %s (attempts=%u)\n",
                          m_items[i].id, m_items[i].attempts);
            removeItemAtUnlocked(i);
            saveManifestUnlocked();
            return true;
        }
    }
    // Pass 3: oldest PENDING (FIFO drop). Skip anything actively UPLOADING — the worker is mid
    // request and pulling the slot would crash on the result-write path.
    for (int i = 0; i < m_count; i++) {
        if (m_items[i].status == QSTATUS_PENDING) {
            Serial.printf("[queue] Evict oldest PENDING %s (FIFO)\n", m_items[i].id);
            deleteFileIfOwnedUnlocked(m_items[i]);
            removeItemAtUnlocked(i);
            saveManifestUnlocked();
            return true;
        }
    }
    return false;
}

uint32_t UploadQueue::jitteredDelayMs(uint32_t baseMs) {
    const uint32_t jitterRange = baseMs / 10u;
    return baseMs + (jitterRange ? (uint32_t)random(jitterRange) : 0u);
}

bool UploadQueue::isRetryable(int httpCode) {
    if (httpCode <= 0) return true;
    if (httpCode == 408) return true;
    if (httpCode == 429) return true;
    if (httpCode == 401) return true;
    if (httpCode >= 500) return true;
    return false;
}

int UploadQueue::forcedIndexUnlocked() const {
    if (!m_forcedItemId[0]) return -1;
    for (int i=0; i<m_count; ++i)
        if (m_items[i].status == QSTATUS_PENDING && !strcmp(m_items[i].id,m_forcedItemId)) return i;
    return -1;
}

UploadQueue::View UploadQueue::view(const char* preferredId, int fallbackIndex) {
    MutexGuard g(m_mutex);
    View v;
    if (!g.taken || !m_items) return v;
    v.count=m_count; v.paused=m_uploadPaused; v.resetPending=m_resetAfterFlight;
    if (!m_count) return v;
    v.index=constrain(fallbackIndex,0,m_count-1);
    if (preferredId && preferredId[0]) for(int i=0;i<m_count;++i)
        if (!strcmp(m_items[i].id,preferredId)) { v.index=i; break; }
    v.item=m_items[v.index];
    v.forced=!strcmp(v.item.id,m_forcedItemId);
    const uint32_t now=millis();
    if (!v.forced) {
        for(uint32_t deadline : {v.item.nextEligibleAt,m_nextNetworkRetryAt,m_nextWorkerStartAt})
            if(upload_queue_policy::deadlinePending(now,deadline)) v.waitMs=max(v.waitMs,deadline-now);
    }
    return v;
}

UploadQueue::ForceResult UploadQueue::forceSend(const char* id) {
    MutexGuard g(m_mutex);
    if (!g.taken || !id || !id[0]) return ForceResult::Missing;
    for(int i=0;i<m_count;++i) {
        QueueItem& item=m_items[i];
        if(strcmp(item.id,id)) continue;
        if(item.status==QSTATUS_UPLOADING) return ForceResult::AlreadyUploading;
        if(forcedIndexUnlocked()>=0 && strcmp(m_forcedItemId,id)) return ForceResult::Busy;
        item.status=QSTATUS_PENDING;
        strlcpy(m_forcedItemId,id,sizeof(m_forcedItemId));
        m_nextWorkerStartAt=0;
        snprintf(m_lastStatus,sizeof(m_lastStatus),"Force send requested: %s",id);
        Serial.printf("[queue-control] force item=%s lifetime=%u turn=%u paused=%u\n",
                      id,item.attempts,item.attemptsThisTurn,m_uploadPaused.load());
        saveManifestUnlocked();
        notifyWorker();
        return ForceResult::Scheduled;
    }
    return ForceResult::Missing;
}

static void clearRetryState(QueueItem& item) {
    item.attempts=0; item.attemptsThisTurn=0;
    item.lastAttemptAt=0; item.nextEligibleAt=0; item.lastError[0]=0;
    if(item.status!=QSTATUS_UPLOADING) item.status=QSTATUS_PENDING;
}

bool UploadQueue::resetRetryHistory() {
    MutexGuard g(m_mutex);
    if(!g.taken || !m_items || !m_count) return false;
    m_resetAfterFlight=false;
    for(int i=0;i<m_count;++i) {
        if(m_items[i].status==QSTATUS_UPLOADING) m_resetAfterFlight=true;
        clearRetryState(m_items[i]);
    }
    m_forcedItemId[0]=0;
    m_nextNetworkRetryAt=0; m_nextWorkerStartAt=0;
    snprintf(m_lastStatus,sizeof(m_lastStatus),"Queue retry history reset");
    Serial.printf("[queue-control] reset count=%d afterFlight=%u paused=%u files=preserved\n",
                  m_count,m_resetAfterFlight,m_uploadPaused.load());
    saveManifestUnlocked();
    notifyWorker();
    return true;
}

bool UploadQueue::applyResetAfterFlightUnlocked(QueueItem& item, bool succeeded) {
    if(!m_resetAfterFlight) return false;
    m_resetAfterFlight=false;
    if(succeeded) return false; // Completed media must not be uploaded twice.
    clearRetryState(item); item.status=QSTATUS_PENDING;
    Serial.printf("[queue-control] reset applied after flight item=%s oldResultIgnored=1\n",item.id);
    return true;
}

#ifdef ORIGAMI_DEV_SERIAL_INPUT
UploadQueue* UploadQueue::controlPreviewForDevelopment() {
    // Isolated RAM fixture: never registered with upload_manager; no SD or HTTP.
    static UploadQueue q;
    q.m_testMode=true;
    if(!q.m_mutex) q.m_mutex=xSemaphoreCreateMutex();
    if(!q.m_items) q.m_items=(QueueItem*)heap_caps_calloc(2,sizeof(QueueItem),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!q.m_mutex || !q.m_items) return nullptr;
    MutexGuard g(q.m_mutex);
    q.m_count=2; q.m_initialized=true; q.m_forcedItemId[0]=0; q.m_resetAfterFlight=false;
    q.m_nextNetworkRetryAt=0; q.m_nextWorkerStartAt=0;
    memset(q.m_items,0,2*sizeof(QueueItem));
    for(int i=0;i<2;++i) {
        snprintf(q.m_items[i].id,sizeof(q.m_items[i].id),"sample-%d",i+1);
        snprintf(q.m_items[i].fileName,sizeof(q.m_items[i].fileName),"Sample clip %d",i+1);
        q.m_items[i].attempts=i?4:12;
        q.m_items[i].nextEligibleAt=millis()+(i?300000u:3600000u);
        strlcpy(q.m_items[i].lastError,"HTTP 503",sizeof(q.m_items[i].lastError));
    }
    return &q;
}

bool UploadQueue::testControlsForDevelopment() {
    UploadQueue q;
    q.m_testMode=true;
    q.m_mutex=xSemaphoreCreateMutex();
    q.m_items=(QueueItem*)heap_caps_calloc(3,sizeof(QueueItem),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!q.m_mutex || !q.m_items) {
        if(q.m_mutex) vSemaphoreDelete(q.m_mutex);
        heap_caps_free(q.m_items); return false;
    }
    q.m_count=3; q.m_initialized=true; q.m_uploadPaused=true;
    for(int i=0;i<3;++i) {
        snprintf(q.m_items[i].id,sizeof(q.m_items[i].id),"test-%d",i);
        snprintf(q.m_items[i].filePath,sizeof(q.m_items[i].filePath),"/test-%d.wav",i);
        q.m_items[i].attempts=12; q.m_items[i].attemptsThisTurn=i==0?2:0;
        q.m_items[i].nextEligibleAt=millis()+3600000u;
        strlcpy(q.m_items[i].lastError,"HTTP 503",sizeof(q.m_items[i].lastError));
    }
    bool pass=true;
    auto check=[&](bool ok,const char* name) {
        Serial.printf("[queue-test] %s pass=%u\n",name,ok); pass &= ok;
    };
    q.m_nextNetworkRetryAt=millis()+60000u;
    check(q.forceSend("missing")==ForceResult::Missing,"stale ID rejected");
    check(q.forceSend("test-1")==ForceResult::Scheduled && q.findRunnableIndexUnlocked(millis())==1,
          "force bypasses global and partial-turn cooldown");
    check(q.m_items[1].attempts==12 && q.m_uploadPaused,"force preserves budget and resource pause");
    check(q.forceSend("test-2")==ForceResult::Busy,"second force cannot replace pending target");
    q.rotateItemToBackUnlocked(0);
    check(q.view("test-1").index==0 && !strcmp(q.view("test-1").item.filePath,"/test-1.wav"),"selection survives rotation");
    q.m_items[0].status=QSTATUS_UPLOADING; q.m_forcedItemId[0]=0;
    check(q.forceSend("test-1")==ForceResult::AlreadyUploading,"no duplicate in-flight request");
    check(q.resetRetryHistory() && q.m_resetAfterFlight && q.m_items[0].status==QSTATUS_UPLOADING,"reset defers active result");
    for(int i=0;i<3;++i) check(q.m_items[i].attempts==0 && q.m_items[i].attemptsThisTurn==0 &&
        q.m_items[i].nextEligibleAt==0 && !q.m_items[i].lastError[0] && q.m_items[i].filePath[0],"reset preserves item and clears history");
    check(q.m_uploadPaused && !q.m_nextNetworkRetryAt && !q.m_nextWorkerStartAt,"reset preserves ownership and clears global waits");
    check(q.applyResetAfterFlightUnlocked(q.m_items[0],false) && q.m_items[0].status==QSTATUS_PENDING &&
          q.m_items[0].attempts==0,"old failure cannot undo reset");
    q.m_items[0].status=QSTATUS_UPLOADING; q.resetRetryHistory();
    check(!q.applyResetAfterFlightUnlocked(q.m_items[0],true) && !q.m_resetAfterFlight,"success still completes after reset");
    q.m_items[0].status=QSTATUS_PENDING; q.forceSend("test-1"); q.removeItemAtUnlocked(0);
    check(!q.m_forcedItemId[0] && q.m_count==2,"removed target cancels force");
    q.m_count=0; check(!q.resetRetryHistory() && q.view("").index<0,"empty queue controls");
    heap_caps_free(q.m_items); vSemaphoreDelete(q.m_mutex);
    return pass;
}
#endif

UploadQueue::Counts UploadQueue::counts() const {
    MutexGuard g(m_mutex);
    return g.taken ? countsUnlocked() : Counts{};
}

UploadQueue::Counts UploadQueue::countsUnlocked() const {
    Counts result;
    if (!m_items) return result;
    result.total = m_count;
    for (int i = 0; i < m_count; i++) {
        if (m_items[i].status == QSTATUS_PENDING) ++result.pending;
        if (m_items[i].status == QSTATUS_FAILED) ++result.failed;
    }
    return result;
}

void UploadQueue::dumpDebugState() {
    MutexGuard g(m_mutex);
    if (!g.taken || !m_items) {
        Serial.println("QUEUE STATUS unavailable");
        return;
    }
    const uint32_t now = millis();
    Serial.printf("QUEUE STATUS count=%d pending=%d worker=%u metadata=PSRAM\n",
                  m_count, countsUnlocked().pending, workerRunning() ? 1u : 0u);
    for (int i = 0; i < m_count; i++) {
        const QueueItem& item = m_items[i];
        const uint32_t waitMs = upload_queue_policy::deadlinePending(now, item.nextEligibleAt)
            ? item.nextEligibleAt - now : 0u;
        Serial.printf("QUEUE ITEM index=%d id=%s status=%u lifetime=%u turn=%u "
                      "waitMs=%lu path=%s error=%s\n",
                      i, item.id, (unsigned)item.status, (unsigned)item.attempts,
                      (unsigned)item.attemptsThisTurn, (unsigned long)waitMs,
                      item.filePath, item.lastError);
    }
}

void UploadQueue::setUploadPaused(bool paused) {
  m_uploadPaused = paused;
}

bool UploadQueue::workerRunning() const {
  portENTER_CRITICAL(&m_workerMux);
  const bool running = m_workerPhase != WorkerPhase::Idle;
  portEXIT_CRITICAL(&m_workerMux);
  return running;
}

void UploadQueue::notifyWorker() {
  portENTER_CRITICAL(&m_workerMux);
  TaskHandle_t worker = m_workerPhase == WorkerPhase::Running ? m_worker : nullptr;
  if (worker) ++m_notifications;
  portEXIT_CRITICAL(&m_workerMux);
  if (!worker) return;
  xTaskNotifyGive(worker);
  portENTER_CRITICAL(&m_workerMux);
  --m_notifications;
  portEXIT_CRITICAL(&m_workerMux);
}

void UploadQueue::retireWorker() {
  portENTER_CRITICAL(&m_workerMux);
  m_workerPhase = WorkerPhase::Retiring;
  portEXIT_CRITICAL(&m_workerMux);
  if (m_networkHeld) {
    m_networkHeld = false;
    if (m_networkRelease) m_networkRelease();
  }
  for (;;) {
    portENTER_CRITICAL(&m_workerMux);
    if (!m_notifications) {
      m_worker = nullptr;
      m_workerPhase = WorkerPhase::Idle;
      portEXIT_CRITICAL(&m_workerMux);
      return;
    }
    portEXIT_CRITICAL(&m_workerMux);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

bool UploadQueue::startWorker() {
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  if(m_testMode) return false;
#endif
  if (!m_initialized) return false;
  if (workerRunning()) return true;
  if (!hasRunnableItem()) return false;
  const uint32_t now = millis();
  if (upload_queue_policy::deadlinePending(now, m_nextWorkerStartAt)) return false;
  portENTER_CRITICAL(&m_workerMux);
  if (m_workerPhase != WorkerPhase::Idle || m_uploadPaused) {
    portEXIT_CRITICAL(&m_workerMux);
    return false;
  }
  m_workerPhase = WorkerPhase::Starting;
  m_workerStop = false;
  portEXIT_CRITICAL(&m_workerMux);
  m_nextWorkerStartAt = 0;

  if (m_networkAcquire && !m_networkAcquire(8000)) {
    m_nextWorkerStartAt = now + 2000u;
    Serial.println("[queue] Network resources busy; worker start deferred");
    retireWorker();
    return false;
  }
  m_networkHeld = m_networkAcquire != nullptr;
  Serial.printf("[queue] Network lease memory: intFree=%u intLargest=%u\n",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  if (m_uploadPaused) {
    retireWorker();
    return false;
  }

  TaskHandle_t created = hal::tasks::spawn(hal::tasks::Id::UploadQ,
                               &UploadQueue::workerTaskTrampoline, this);
  if (!created) {
    m_nextWorkerStartAt = now + 5000u;
    Serial.printf("[queue] Upload worker spawn deferred: intFree=%u intLargest=%u psramFree=%u psramLargest=%u\n",
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
    retireWorker();
    return false;
  }
  portENTER_CRITICAL(&m_workerMux);
  m_worker = created;
  m_workerPhase = WorkerPhase::Running;
  portEXIT_CRITICAL(&m_workerMux);
  notifyWorker();
  return true;
}

void UploadQueue::requestStopForVoice() {
  portENTER_CRITICAL(&m_workerMux);
  m_uploadPaused = true;
  m_workerStop = true;
  const bool idle = m_workerPhase == WorkerPhase::Idle;
  portEXIT_CRITICAL(&m_workerMux);
  if (idle) return;
  notifyWorker();
}

bool UploadQueue::stopWorkerForVoice(uint32_t timeoutMs) {
  requestStopForVoice();
  if (!workerRunning()) return true;
  const uint32_t startedMs = millis();
  while (workerRunning() && (uint32_t)(millis() - startedMs) < timeoutMs)
    vTaskDelay(pdMS_TO_TICKS(10));
  if (workerRunning()) {
    portENTER_CRITICAL(&m_workerMux);
    m_workerStop = false;
    m_uploadPaused = false;
    portEXIT_CRITICAL(&m_workerMux);
    Serial.printf("[queue] Voice pause timed out after %lu ms\n",
                  (unsigned long)(millis() - startedMs));
    return false;
  }
  Serial.printf("[queue] Worker stopped for voice in %lu ms\n",
                (unsigned long)(millis() - startedMs));
  return true;
}

bool UploadQueue::resumeWorker() {
  m_uploadPaused = false;
  // The service tick starts a pending worker. Keeping this nonblocking matters
  // because voice and music release paths run on the UI task.
  return true;
}

void UploadQueue::clearPendingItems() {
  if (!m_initialized) return;
  MutexGuard g(m_mutex);
  bool changed = false;
  for (int i = m_count - 1; i >= 0; i--) {
    if (m_items[i].status != QSTATUS_PENDING) continue;
    Serial.printf("[queue] Dropping pending %s for voice priority\n", m_items[i].id);
    deleteFileIfOwnedUnlocked(m_items[i]);
    removeItemAtUnlocked(i);
    changed = true;
  }
  if (changed) {
    saveManifestUnlocked();
    snprintf(m_lastStatus, sizeof(m_lastStatus), "Pending cleared (voice)");
  }
}

void UploadQueue::cancelAll() {
  MutexGuard g(m_mutex);
  m_forcedItemId[0]=0; m_resetAfterFlight=false;
  for (int i = 0; i < m_count; i++) {
    deleteFileIfOwnedUnlocked(m_items[i]);
  }
  m_count = 0;
  saveManifestUnlocked();
  snprintf(m_lastStatus, sizeof(m_lastStatus), "Queue cleared");
  Serial.println("[queue] All items cancelled");
}

bool UploadQueue::removeByFilePath(const char* filePath) {
  if (!m_initialized || !filePath || !filePath[0]) return false;
  MutexGuard g(m_mutex);
  for (int i = 0; i < m_count; i++) {
    if (strcmp(m_items[i].filePath, filePath) != 0) continue;
    if (m_items[i].status == QSTATUS_UPLOADING) return false;
    Serial.printf("[queue] Removing item for deleted file: %s\n", filePath);
    removeItemAtUnlocked(i);
    saveManifestUnlocked();
    snprintf(m_lastStatus, sizeof(m_lastStatus), "Recording deleted");
    return true;
  }
  return false;
}

bool UploadQueue::parseUrl(const char* url, char* host, size_t hostLen,
                           uint16_t* port, char* path, size_t pathLen) {
    const char* p = url;
    *port = 80;
    host[0] = '\0';
    path[0] = '\0';

    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
        *port = 443;
    }

    const char* colon = strchr(p, ':');
    const char* slash = strchr(p, '/');

    if (colon && (!slash || colon < slash)) {
        size_t hLen = colon - p;
        if (hLen >= hostLen) hLen = hostLen - 1;
        strncpy(host, p, hLen);
        host[hLen] = '\0';
        *port = (uint16_t)atoi(colon + 1);
        if (slash) {
            strlcpy(path, slash, pathLen);
        }
    } else if (slash) {
        size_t hLen = slash - p;
        if (hLen >= hostLen) hLen = hostLen - 1;
        strncpy(host, p, hLen);
        host[hLen] = '\0';
        strlcpy(path, slash, pathLen);
    } else {
        strlcpy(host, p, hostLen);
    }

    size_t pLen = strlen(path);
    if (pLen == 1 && path[0] == '/') {
        path[0] = '\0';
    }

    return host[0] != '\0';
}
