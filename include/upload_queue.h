#pragma once

#include <Arduino.h>
#include <atomic>
#include <freertos/portmacro.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#define UPLOAD_QUEUE_MAX_ITEMS 20
#define UPLOAD_QUEUE_DIR       "/upload_queue"
#define UPLOAD_QUEUE_MANIFEST  "/upload_queue/manifest.json"

enum QueueItemStatus : uint8_t {
    QSTATUS_PENDING   = 0,
    QSTATUS_UPLOADING = 1,
    QSTATUS_COMPLETED = 2,
    QSTATUS_FAILED    = 3
};

struct QueueItem {
    char id[24];
    char type[8];           // "image" or "audio"
    char filePath[96];
    char mimeType[24];
    char fileName[32];
    uint32_t createdAt;
    uint32_t lastAttemptAt;
    uint32_t nextEligibleAt;
    uint8_t  attempts;          // Lifetime attempts; persisted across reboot.
    uint8_t  attemptsThisTurn;  // Runtime-only; reset after rotation/reboot.
    QueueItemStatus status;
    bool deleteAfterUpload;
    char lastError[48];
};

static_assert(sizeof(QueueItem) == 248,
              "QueueItem growth permanently increases PSRAM usage");

namespace services { namespace uploads { bool ensureInit(); void tick(); } }

class UploadQueue {
public:
    using NetworkAcquireCallback = bool (*)(uint32_t timeoutMs);
    using NetworkReleaseCallback = void (*)();

    // Generate a unique file path inside the queue directory.
    // prefix: "IMG" or "AUD"
    // ext: ".jpg" or ".wav"
    // Writes full path to outPath (e.g. "/upload_queue/IMG_00012.jpg")
    // Returns the sequence number, or 0 on failure.
    uint32_t generatePath(const char* prefix, const char* ext,
                          char* outPath, size_t outPathSize);

    // Register a file already written to SD.
    // type: "image" or "audio"
    // filePath: full SD path
    // mimeType: e.g. "image/jpeg", "audio/wav"
    // deleteAfterUpload: true for queue-owned temporary files; false for retained media
    bool enqueue(const char* type, const char* filePath, const char* mimeType,
                 bool deleteAfterUpload = true);

    // One mutex-protected observation; pending excludes active transfers.
    struct Counts { int total = 0, pending = 0, failed = 0; };
    Counts counts() const;
    int  size()         const { return counts().total; }
    int  pendingCount() const { return counts().pending; }
    int  failedCount()  const { return counts().failed; }
    bool workerRunning() const;
    const char* lastStatus() const { return m_lastStatus; }

    /** Print queue order and retry state for on-device diagnostics. */
    void dumpDebugState();

    struct View {
        QueueItem item{};
        int count = 0, index = -1;
        uint32_t waitMs = 0;
        bool paused = false, forced = false, resetPending = false;
    };
    // Copies a stable item under the queue mutex; IDs survive queue rotations.
    View view(const char* preferredId, int fallbackIndex = 0);
    enum class ForceResult { Scheduled, Missing, AlreadyUploading, Busy };
    ForceResult forceSend(const char* id);
    // Clears retry history, not files. An active transfer finishes normally.
    bool resetRetryHistory();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    static bool testControlsForDevelopment();
    static UploadQueue* controlPreviewForDevelopment();
#endif

    // Remove all items and delete only queue-owned files.
    void cancelAll();

    // Remove a queued item that references filePath without deleting the file.
    bool removeByFilePath(const char* filePath);

    /**
     * When true, the upload worker will not start new transfers (Kami voice / PSRAM has priority).
     * In-flight upload (if any) may still finish one HTTP round.
     */
    void setUploadPaused(bool paused);
    bool uploadPaused() const { return m_uploadPaused; }

    /**
     * Pause uploads and wait for worker/resource quiescence. Stack reclamation
     * follows asynchronously in the RTOS idle task.
     * Returns false if an in-flight transfer did not finish before timeoutMs.
     */
    bool stopWorkerForVoice(uint32_t timeoutMs);
    /** Request cooperative shutdown without waiting on HTTP on the UI task. */
    void requestStopForVoice();

    /** Recreate the upload worker after a voice interaction. */
    bool resumeWorker();

    /** Drop all PENDING items. Queue-owned SD files are deleted; retained files survive. */
    void clearPendingItems();

private:
    enum class UploadResultKind : uint8_t {
        HttpResponse,
        RetryableFailure,
        PermanentItemFailure,
        GlobalDeferral,
        Aborted
    };

    struct UploadResult {
        UploadResultKind kind;
        int httpCode;
        bool attemptStarted;
        uint32_t deferMs;
    };

    // Fixed-capacity backing storage allocated once from 8-bit-capable PSRAM.
    // Keeping only the pointer here recovers about 5 KB of permanent internal
    // DRAM while preserving deterministic capacity and in-place array behavior.
    QueueItem* m_items = nullptr;
    int      m_count       = 0;
    uint32_t m_nextImgId   = 1;
    uint32_t m_nextAudId   = 1;
    char     m_lastStatus[64];
    bool     m_initialized = false;
  /** Network/server unreachable backoff (do not hammer backend). */
  uint32_t m_nextNetworkRetryAt = 0;
  /** Worker spawn backoff when internal RAM cannot allocate the task stack. */
  uint32_t m_nextWorkerStartAt = 0;
    char m_forcedItemId[24] = {};
    bool m_resetAfterFlight = false;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
    bool m_testMode = false;
#endif

    /**
     * Mutex around all shared mutable state (m_items, m_count, m_lastStatus,
     * counters, manifest writes). Held briefly by main thread (enqueue,
     * lookups) and by the worker task between phases of an upload — never
     * around the actual TCP read/write inside doUpload(), so the worker
     * being mid-upload doesn't block enqueue from main.
     */
    SemaphoreHandle_t m_mutex   = nullptr;
    enum class WorkerPhase { Idle, Starting, Running, Retiring };
    mutable portMUX_TYPE m_workerMux = portMUX_INITIALIZER_UNLOCKED;
    WorkerPhase m_workerPhase = WorkerPhase::Idle;
    TaskHandle_t m_worker = nullptr;
    unsigned m_notifications = 0;
    std::atomic<bool> m_workerStop{false};
    std::atomic<bool> m_uploadPaused{false};
    bool m_networkHeld = false; // Starting owner, then published worker only.
    void notifyWorker();
    void retireWorker();
    NetworkAcquireCallback m_networkAcquire = nullptr;
    NetworkReleaseCallback m_networkRelease = nullptr;

    Counts countsUnlocked() const; // Caller holds m_mutex.
    bool loadManifestUnlocked();
    bool loadManifestFileUnlocked(const char* path);
    bool saveManifestUnlocked();
    int  findRunnableIndexUnlocked(uint32_t now) const;
    int forcedIndexUnlocked() const;
    bool applyResetAfterFlightUnlocked(QueueItem& item, bool succeeded);
    bool hasRunnableItem();
    /**
     * Make space for one new item by evicting the oldest expendable entry, in this priority order:
     *   1) FAILED items (already given up — pure noise in the queue).
     *   2) PENDING items that have exhausted their retry budget (about to fail anyway).
     *   3) The oldest PENDING item (FIFO drop — last resort).
     * Returns true if a slot was freed (or already free), false if the queue can't make room.
     * Caller MUST hold `m_mutex`. The evicted file's SD copy is also deleted to reclaim space.
     */
    bool makeRoomForOneUnlocked();
    UploadResult doUpload(QueueItem& item);
    uint32_t jitteredDelayMs(uint32_t baseMs);
    bool isRetryable(int httpCode);
    void deleteFileIfOwnedUnlocked(const QueueItem& item);
    void removeItemAtUnlocked(int index);
    void rotateItemToBackUnlocked(int index);
    bool parseUrl(const char* url, char* host, size_t hostLen,
                  uint16_t* port, char* path, size_t pathLen);

    // The service serializes setup and publishes readiness to other tasks.
    friend bool services::uploads::ensureInit();
    /**
     * Install callbacks that quiesce competing TLS users around each upload.
     * Call before init(). Acquire runs on svc_task before worker creation;
     * release runs on uploadq cleanup, or svc_task if creation fails.
     */
    void setNetworkCoordinator(NetworkAcquireCallback acquire,
                               NetworkReleaseCallback release) {
        m_networkAcquire = acquire;
        m_networkRelease = release;
    }

    // Load or create queue on SD card
    bool init();

    // Only the admitted background service may tick/start the worker.
    // Network acquisition can block; never run this from a view tick.
    friend void services::uploads::tick();
    void tick();

    // Worker plumbing.
    void  workerLoop();
    bool  processOnePending();
    bool  startWorker();
    static void workerTaskTrampoline(void* arg);
};
