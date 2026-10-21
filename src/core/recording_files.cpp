#include "core/recording_files.h"
#include <Arduino.h>
#include <SD.h>
#include <cstring>
#include <time.h>
#include "core/board_time.h"
#include "hal/hal_bus.h"
#include "hal/hal_sd.h"

namespace core { namespace recording_files {
bool makePath(const char* directory, const char* prefix, char* out, size_t capacity) {
    if (!out || !capacity) return false;
    out[0] = 0;
    if (!directory || directory[0] != '/' || !prefix) return false;
    char parents[128];
    const int copied = snprintf(parents, sizeof(parents), "%s", directory);
    if (copied < 0 || (size_t)copied >= sizeof(parents)) return false;
    for (char* p = parents + 1; *p; ++p) {
        if (*p != '/') continue;
        *p = 0;
        const bool ok = hal::sd::ensureDir(parents);
        *p = '/';
        if (!ok) return false;
    }
    if (!hal::sd::ensureDir(parents)) return false;
    char stem[128];
    struct tm local{};
    const time_t now = ::time(nullptr);
    int n;
    if (core::time::isSynced() && core::time::timezoneConfigured() && localtime_r(&now, &local)) {
        n = snprintf(stem, sizeof(stem), "%s/%s%04d%02d%02d_%02d%02d%02d", directory, prefix,
                     local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                     local.tm_hour, local.tm_min, local.tm_sec);
    } else {
        n = snprintf(stem, sizeof(stem), "%s/%sBOOT_%lu", directory, prefix, (unsigned long)millis());
    }
    if (n < 0 || (size_t)n >= sizeof(stem)) return false;
    for (int suffix = 0; suffix <= 99; ++suffix) {
        n = suffix ? snprintf(out, capacity, "%s_%02d.wav", stem, suffix)
                   : snprintf(out, capacity, "%s.wav", stem);
        if (n < 0 || (size_t)n >= capacity) break;
        if (!hal::sd::exists(out)) return true;
    }
    out[0] = 0;
    return false;
}

bool hasAudio(const char* path) {
    if (!path || !path[0]) return false;
    hal::bus::lockSpi();
    File file = SD.open(path, FILE_READ);
    const bool result = file && file.size() > 44u;
    if (file) file.close();
    hal::bus::unlockSpi();
    return result;
}
} }
