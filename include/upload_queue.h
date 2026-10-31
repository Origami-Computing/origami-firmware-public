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
    enum class VideoEnqueue { Queued, AlreadyQueued, Busy, Full, Invalid, AccountRequired, AccountChanged, StorageUnavailable };
    // Explicit request for a finalized /videos/<name>.avi original. Brief SD
    // setup/manifest I/O; use a workflow action/service context, never a painter.
    // Captures the current backend/account binding before publishing the item.
    // Originals are never queue-owned. Same pending item/account is idempotent; a fresh
    // requeue resets old resume metadata and rehashes on the worker. Busy means
    // a cancelled operation still owns this ID; retry after it acknowledges stop.
    // StorageUnavailable can be an ambiguous manifest write: media is retained,
    // and boot recovery may discover a successfully persisted pending request.
    VideoEnqueue enqueueVideo(const char* filePath);
    enum class MutationResult { Ready, Busy, Invalid, StorageUnavailable };
    class FileMutation {
    public:
        // q must be initialized. Reserves a canonical file/directory subtree
        // against enqueue without retaining the queue mutex during file/network
        // I/O. removePending=false refuses queued sources; true cancels pending
        // references and confirms the manifest before allowing deletion. Active
        // native operations always return Busy, including after queue clearing.
        // Exact video-source mutations invalidate prior resume metadata before
        // changing bytes, so backup manifests cannot reuse a stale receipt.
        FileMutation(UploadQueue& q,const char* path,bool removePending);
        ~FileMutation();
        FileMutation(const FileMutation&)=delete;
        FileMutation& operator=(const FileMutation&)=delete;
        bool ready() const {return result_==MutationResult::Ready;}
        MutationResult result() const {return result_;}
        int removed() const {return removed_;}
    private:
        UploadQueue* queue_=nullptr;
        MutationResult result_=MutationResult::Invalid;
        int removed_=0;
    };

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
        Aborted,
        // Durable chunk/session progress or pending verification, never a final
        // receipt. Keep the item and media, yield the network lease, rotate fairly.
        Progress
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
    // Retained through cancel/removal until doUpload returns. Prevents a new
    // item or resume record reusing the identity of an in-flight completion.
    char m_inFlightId[24] = {};
    char m_inFlightPath[96] = {};
    char m_mutationPath[96] = {};
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
    UploadResult doVideoUpload(QueueItem& item);
    bool mutationBlocksUnlocked(const char* path) const;
    MutationResult beginFileMutation(const char* path,bool removePending,int& removed);
    void endFileMutation();
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
