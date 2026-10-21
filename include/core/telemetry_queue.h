#pragma once

#include <stdint.h>

/**
 * @file telemetry_queue.h
 * @brief Durable, bounded event queue for device telemetry.
 *
 * A passive store (no worker task of its own) — net_poll (Core 0) drains it.
 * Events live in a mutex-protected RAM ring. Warnings, errors and crashes
 * are also journaled to SD; diagnostics and heartbeats remain RAM-only.
 * When full, new events are dropped to preserve the in-flight snapshot.
 * Diagnostics leave eight slots available for durable events.
 *
 * Threading: enqueue() is called from any task (LOGE/LOGW, crash report,
 * heartbeat). The drain API (snapshot/ack) is called only from net_poll.
 * All access is guarded by an internal mutex.
 */

namespace core {
namespace telemetry_queue {

enum Kind : uint8_t {
  KIND_VERBOSE = 3, // RAM-only diagnostic logs; reserve capacity for durable events
  KIND_LOG   = 0,  // a single log entry, batched into POST /api/device/logs
  KIND_CRASH = 1,  // a full crash object, POSTed as-is to /api/device/crashes
  KIND_HEARTBEAT = 2,  // a heartbeat object, POSTed to /api/device/heartbeat
};

constexpr int kMaxItems   = 512;
constexpr int kItemJsonCap = 240;  // one event's JSON payload

struct Item {
  uint8_t kind;
  char    json[kItemJsonCap];  // for KIND_LOG: {"ts","level","tag","msg"}; else full body
};

/** Create the mutex, ensure the SD dir, and replay any journaled (unsent)
 *  events into the RAM ring. Safe to call once at boot. */
void init();

/** Append one event. `json` is the payload for `kind`. Thread-safe; drops the
 *  incoming event if its capacity is exhausted. Durable events get a best-effort SD journal append. */
void enqueue(Kind kind, const char* json);

/** True if any events are pending (cheap, mutex-guarded). */
bool hasPending();

/** Copy up to `maxN` pending items (oldest first) into `out` without removing
 *  them. Returns the count copied. net_poll POSTs them, then calls ack(). */
int snapshot(Item* out, int maxN);

/** Remove the oldest `n` items (the ones just POSTed successfully) and rewrite
 *  the SD journal to match. */
void ack(int n);

// Cumulative since boot, including capacity and lock-contention loss.
struct Stats { uint32_t queued, droppedVerbose, droppedDurable; };
Stats stats();

}  // namespace telemetry_queue
}  // namespace core
