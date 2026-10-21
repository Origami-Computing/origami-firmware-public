#pragma once

#include <stddef.h>
#include <stdint.h>

namespace widgets {
namespace sync {

/**
 * Pull all device widgets from the backend onto the SD card.
 *
 * GET <backend>/api/device-widgets (X-Api-Key auth) for the manifest, then
 * each script body, writing /widgets/<slug>/main.lua. Synchronous — blocks
 * the calling task for the duration of the HTTP round-trips.
 *
 * Returns the number of widgets written, or -1 on error (errOut filled with
 * a short human-readable reason).
 */
int syncFromBackend(char* errOut, size_t errLen);

/**
 * Fetch just ONE widget (by backend id) and write it to /widgets/<slug>/.
 * Used by the push-driven launch flow (widget_ready) to pull the freshly
 * generated widget without a full manifest sync. Returns 0 on success, -1 on
 * error (errOut filled). Synchronous; call from ui_task.
 */
int syncOne(const char* id, const char* slug, char* errOut, size_t errLen);

/**
 * Spawn the background poll task (widget_poll, Core 0, PRIO_SERVICE).
 * Call once from services::startBackgroundTasks() after WiFi task is up.
 * The task wakes every WIDGET_POLL_INTERVAL_MS, fetches the manifest, and
 * calls syncFromBackend() only when the manifest has changed.
 */
void startPolling();

/** Wake the existing worker for foreground data; does not bypass admission/rate limits. */
void requestPoll();

/**
 * Cooperatively pause the background poller at a point where it owns no
 * HTTP/TLS or SD resources. Returns false if an in-flight request does not
 * reach a checkpoint before timeoutMs.
 */
bool suspendPolling(uint32_t timeoutMs = 5000);
/** Nonblocking foreground handover; query acknowledgement before starting TLS. */
void requestPollingPause();
bool pollingPaused();

/** Resume a poller paused by suspendPolling(). */
void resumePolling();

/**
 * Returns true (and clears the flag) if a background sync completed since
 * the last call. Safe to call from ui_task — written by widget_poll task
 * using atomic publication and read-clear. Multiple completions coalesce.
 */
bool takeSyncNotification();

}  // namespace sync
}  // namespace widgets
