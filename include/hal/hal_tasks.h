#pragma once

/**
 * @file hal_tasks.h
 * @brief Central task registry for all FreeRTOS tasks in the firmware.
 *
 * Every task that runs on this device is described here: name, core, priority,
 * and stack size.  Call sites use hal::tasks::spawn() instead of calling
 * xTaskCreatePinnedToCore directly, so the entire concurrency model is visible
 * in one place.
 *
 * Priority model (FreeRTOS: higher number = higher urgency):
 *
 *   PRIO_BACKGROUND (1) — work that can always yield: file upload I/O
 *   PRIO_SERVICE    (2) — periodic background services: WiFi, OTA check, QR decode
 *   PRIO_NETWORK    (4) — persistent user-facing network transport: kami_ws
 *   PRIO_REALTIME   (5) — display pipeline and audio playback
 *
 * Core assignments:
 *
 *   CORE_UI  (1) — owns the display pipeline; no blocking I/O allowed here
 *   CORE_SVC (0) — all background and network tasks
 *
 * ESP-IDF reserves priorities 19-24 for its own tasks (WiFi driver, timer,
 * event loop).  Keep application tasks at or below PRIO_REALTIME (5).
 */

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace hal {
namespace tasks {

// ── Priority levels ──────────────────────────────────────────────────────────
// Use these names everywhere — never raw integers.

static constexpr UBaseType_t PRIO_BACKGROUND = 1;  // uploadq
static constexpr UBaseType_t PRIO_SERVICE    = 2;  // wifi_mgr, svc, qr_scan
static constexpr UBaseType_t PRIO_NETWORK    = 4;  // kami_ws (user waiting for reply)
static constexpr UBaseType_t PRIO_REALTIME   = 5;  // ui_task, mp3_play

// ── Core assignments ─────────────────────────────────────────────────────────
static constexpr BaseType_t CORE_UI  = 1;  // display pipeline / display-safe helpers
static constexpr BaseType_t CORE_SVC = 0;  // all background tasks

// ── Task identifiers ─────────────────────────────────────────────────────────
// One entry per FreeRTOS task.  The enum order must match kSpecs[] in hal_tasks.cpp.

enum class Id : uint8_t {
    // Core 1
    Ui,         // Display pipeline: input → nav → draw → flush

    // Core 0
    WifiMgr,    // WiFi boot, join backoff, pending-restore
    Services,   // BLE service, automation, idle / sleep policy
    UploadQ,    // HTTP multipart file-upload worker
    KamiSend,   // Persistent agent WebSocket + streamed PCM/replies
    QrScan,     // Camera QR decode (quirc)
    Mp3Play,    // MP3 decode + I2S DMA playback
    WavPlay,    // WAV SD stream + I2S DMA playback
    NetPoll,    // Unified periodic HTTP: automations GET, telemetry/heartbeat POST, OTA check
    WidgetPoll, // Periodic manifest poll → auto-sync Lua widgets from backend

    OtaDownload, // Signed HTTPS firmware download and flash worker

    OtaNotice, // Background Settings update lookup

    Haptic, // Retained 1 ms motor modulation worker, asleep between effects


    VideoWrite, // Camera JPEG queue -> SD AVI; only while recording/finalizing
    VideoRead,  // SD AVI -> bounded JPEG queue; decoded/presented on UI
    VideoMic, // Concurrent video microphone RX; same camera_audio task profile
    VideoCapture, // Exclusive camera reader -> recording queue / latest preview
    VideoPreview, // Optional camera/playback JPEG decode; no display access
    _Count      // sentinel — do not use
};

// ── Per-task specification ───────────────────────────────────────────────────

struct Spec {
    const char*   name;        // FreeRTOS task name (shown in monitor, WDT logs)
    BaseType_t    core;        // CORE_UI or CORE_SVC
    UBaseType_t   priority;    // PRIO_* constant
    uint32_t      stackBytes;  // task stack in bytes
};

// ── Public API ───────────────────────────────────────────────────────────────

/** Return the Spec for the given task id. */
const Spec& spec(Id id);

/**
 * Create a FreeRTOS task using the centrally-registered spec.
 *
 * @param id    Task identifier (must match the entry in kSpecs[]).
 * @param fn    Task function (signature: void fn(void*)).
 * @param arg   Optional argument forwarded to fn.
 * @return      Task handle on success; nullptr on allocation failure.
 *
 * Logs the spawn (name, core, priority, stack) on success, and an error on
 * failure — useful for catching priority / stack misconfiguration at boot.
 */
TaskHandle_t spawn(Id id, TaskFunction_t fn, void* arg = nullptr);

/** Self-delete after publishing service quiescence. UploadQ/NetPoll/OtaDownload/VideoCapture must use
 * this instead of vTaskDelete: they share one retained 12 KB internal stack,
 * reusable only after same-core idle cleanup. VideoCapture additionally holds
 * its Camera Session peripheral reservation for the entire worker lifetime.
 * A premature spawn returns null.
 * Mp3Play/WavPlay similarly share a separate 3584-byte internal slot.
 * Other tasks retain ordinary dynamic allocation/deletion. Never returns. */
void deleteCurrent(Id id);

/** Worker-only, after its final work, before retirement publication/deletion.
 * Saves a value instead of lending its short-lived task handle to diagnostics. */
void recordCurrentStack(Id id);
/** Print live boot-task watermarks or explicitly labelled completed-worker samples. */
void dumpStackWatermarks();

}  // namespace tasks
}  // namespace hal
