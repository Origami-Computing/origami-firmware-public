#pragma once
#include <stdint.h>
namespace core { namespace usage {
// Initialize once before tasks. No user content is accepted. Callers use stable
// app/event IDs. Bounded queue, best-effort SD persistence; counters expose loss.
void init();
void emit(const char* app, const char* event, int64_t durationMs = -1);
} }
