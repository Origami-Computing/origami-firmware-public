#pragma once

#include <stdint.h>
#include <time.h>

#include "core/morning_quote_record.h"

namespace services {
namespace morning_quote {

enum class Status : uint8_t {
  Idle,
  WaitingForNetwork,
  Fetching,
  Ready,
  Error,
};

/** Load the two date-keyed quotes (today/upcoming) from NVS. */
void begin();

/** Prepare a quote for the local calendar date containing epochSeconds. */
void requestForEpoch(time_t epochSeconds, bool force = false);

/** Prepare today's quote. Used by alarm start and explicit retry. */
void requestToday(bool force = false);

/** Collect agent text results and persist the cache on svc_task. */
void serviceBackground(uint32_t nowMs);

bool getForToday(core::morning_quote_record::Record* out);
bool getForDay(uint32_t dayKey, core::morning_quote_record::Record* out);
uint32_t localDayKey(time_t epochSeconds);

Status status();
const char* statusText();
bool fetching();
/** Hold quote work and wait for cache IO to finish before peripheral teardown.
 * Agent transport owns its socket and is paused separately by the coordinator. */
bool suspendForPeripheralIo(uint32_t timeoutMs);
void resumeAfterPeripheralIo();

#ifdef ORIGAMI_DEV_SERIAL_INPUT
/** Seed a deterministic current-day record through the real NVS/SD path. */
bool seedTodayForDevelopment();
/** Simulate no cached quote in RAM only; reboot/begin restores saved records. */
bool missCacheForDevelopment();
/** Use RAM-only fixture quotes; restore reloads NVS and is safe to repeat. */
bool fixtureCacheForDevelopment(bool restore);
/** Restore two validated records from the explicit SD recovery file. */
bool restoreCacheFileForDevelopment();
#endif

}  // namespace morning_quote
}  // namespace services
