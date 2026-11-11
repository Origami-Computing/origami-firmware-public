#pragma once
#include <stdint.h>

namespace services { namespace onboarding_progress {
struct Progress { unsigned page = 0; bool completed = false; bool offered = false; unsigned interest = 0; bool account = false, joinWifi = false, setupPublished = false, connected = false; };
// Persist first presentation independently of completion; unfinished tutorials resume on reboot.
bool markOffered();
// UI-task only. Owns the v7 record and completed-state migration from older tutorials and last-read/successful-write cache.
// Failed reads return fresh progress without replacing that cache.
Progress load();
// Normalize transient checkpoints for storage. Failed writes remain retryable.
bool save(unsigned page, bool completed, unsigned interest=0, bool account=false, bool joinWifi=false, bool setupPublished=false, bool connected=false);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
// Writes the fresh record without changing the cache; the next load refreshes it.
bool resetForDevelopment();
uint32_t savedRecordForDevelopment();
#endif
}} // namespace services::onboarding_progress
