#pragma once
#include <stddef.h>

namespace core { namespace recording_files {
// Timestamped WAV name (or BOOT uptime when local time is unavailable).
// Prefix and directory are caller policy; collision suffixes are shared.
// Returns false on storage/path failure or exhaustion of 100 candidate names.
// The caller holds its storage reservation through open; this is not a file lock.
bool makePath(const char* directory, const char* prefix, char* out, size_t capacity);
bool hasAudio(const char* path);
} }
