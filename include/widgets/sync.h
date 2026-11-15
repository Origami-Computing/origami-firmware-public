#pragma once

#include <stddef.h>
#include "services/custom_apps.h"
#include <stdint.h>
#include "core/app_store_qr.h"

namespace widgets {
namespace sync {

/**
 * Fetch ONE widget by backend ID and install a validated persistent package.
 * Used by the push-driven launch flow (widget_ready) to pull the freshly
 * generated widget without a full manifest sync. Returns 0 on success, -1 on
 * error (errOut filled). Worker-only; UI callers submit through widgets::storage.
 */
int syncOne(const char* id, const char* slug, char* errOut, size_t errLen,
            services::custom_apps::Identity* installed = nullptr);

// Existing widget worker only, after network admission. No credentials are sent.
// On success, a numeric request is replaced with its resolved URL/hash so
// storage uses the same namespace as a direct package QR. Failure leaves it intact.
services::custom_apps::Result download(core::app_store::Download&,
    services::custom_apps::Package&, services::custom_apps::Cancel);

/**
 * Spawn the background poll task (widget_poll, Core 0, PRIO_SERVICE).
 * Call once from services::startBackgroundTasks() after WiFi task is up.
 * Services foreground app downloads, targeted launches, and live app data.
 */
bool startPolling(); // false if worker creation failed; safe to retry

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

}  // namespace sync
}  // namespace widgets
