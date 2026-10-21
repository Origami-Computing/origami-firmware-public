#pragma once

#include <stdint.h>

/**
 * @file device_fs.h
 * @brief Device-side executor for the "sync to web" file bridge.
 *
 * The web app cannot reach the device directly (home NAT), so it enqueues file
 * operations on the server and the device drains them by polling
 * GET /api/device/fs/ops. Ops:
 *   - manifest : walk the SD card and POST the file list; the server reconciles
 *                it against the cloud mirror and enqueues push/pull/delete ops.
 *   - push     : stream an SD file up to the server (device -> cloud).
 *   - pull     : download a file from a signed URL and write it to SD (cloud ->
 *                device).
 *   - delete   : remove a file or folder subtree from SD.
 *   - mkdir    : create a folder on SD.
 *   - list     : list a single directory (lightweight; manifest covers the tree).
 *
 * Runs INSIDE the net_poll task (Core 0), so it never blocks the UI core and
 * automatically pauses during voice/music (those stop net_poll). Long transfers
 * poll services::net_poll::stopRequested() and abort promptly so stop() can quiesce.
 * Push transfers hold an acknowledged Agent transport pause until their TLS
 * client is destroyed, reserving internal/DMA memory for the upload. Active
 * foreground Agent sessions and failed pause admission defer the server lease.
 * Pending Agent text turns also defer uploads and interrupt an in-flight push
 * at its next chunk, allowing the paused socket to resume without starvation.
 * JSON operation batches/manifests allocate in PSRAM so their retained strings
 * do not consume internal memory needed by TLS DMA and audio startup.
 *
 * The `/upload_queue` folder is EXCLUDED from every operation so this never
 * races the timeline upload worker.
 */

namespace services {
namespace device_fs {

// Drain up to a few queued ops for this device (one poll cycle). Blocking; call
// from the net_poll task when WiFi is connected and a device token is held.
// No-op (returns false) if not registered yet.
bool poll();

enum class SyncRequest { Queued, Interactive };
// Coalesce a full manifest request and publish Starting as one operation.
// Interactive retains Settings' immediate poller wake attempt; Queued (serial)
// leaves worker activation to existing policy. A busy/failed wake leaves the
// request pending until polling resumes; this does not grant a new reservation.
// SD walking and HTTP remain on net_poll, never on the requesting task.
void requestSync(SyncRequest request = SyncRequest::Queued);

struct SyncStatus {
  bool active;
  bool done;
  bool ok;
  uint32_t value;
  uint32_t max;
  uint32_t updated_ms;
  char phase[32];
  char detail[64];
};

void getSyncStatus(SyncStatus& out);

}  // namespace device_fs
}  // namespace services
