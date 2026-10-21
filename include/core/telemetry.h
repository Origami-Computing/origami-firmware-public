#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @file telemetry.h
 * @brief Lightweight logging facade with remote forwarding + crash breadcrumb.
 *
 * LOGE/LOGW/LOGI/LOGD/LOGV behave like Serial.printf with a [tag] prefix (preserving the
 * existing log convention) and additionally:
 *   - push the line into a small RAM ring buffer (recent context for crashes),
 *   - record the last tag into an RTC_NOINIT breadcrumb that survives a panic /
 *     watchdog reset (read back on the next boot by crash_report),
 *   - enqueue a structured event to the telemetry queue so
 *     net_poll ships it to /api/device/logs.
 *
 * INFO/DEBUG/VERBOSE forward through a bounded RAM-only queue; WARN/ERROR are durable.
 */

namespace core {
namespace telemetry {

enum class Level : uint8_t { Info = 0, Warn = 1, Error = 2, Debug = 3, Verbose = 4 };

/** Initialize the RAM ring mutex. Call once at boot (before background tasks).
 *  The RTC breadcrumb is intentionally NOT cleared here — crash_report reads it
 *  first, then calls clearBreadcrumbCrash(). */
void init();

/** Core entry point. Prefer the LOGE/LOGW/LOGI/LOGD/LOGV macros. */
void log(Level level, const char* tag, const char* fmt, ...)
    __attribute__((format(printf, 3, 4)));

// ── Crash breadcrumb (survives panic / WDT reset via RTC_NOINIT memory) ──────

/** Record the name of the task that the watchdog found hung, just before reset.
 *  Safe to call from a TWDT timeout callback. */
void setBreadcrumbHungTask(const char* taskName);

/** True if the breadcrumb magic is valid (i.e. RAM survived a soft reset). */
bool breadcrumbValid();

/** Last tag logged before the reset (empty if none). */
const char* breadcrumbLastTag();

/** Hung-task name recorded by the TWDT callback (empty if the reset wasn't a
 *  watchdog hang). */
const char* breadcrumbHungTask();

/** Join the most recent buffered log lines (newest last) into `out`, for
 *  attaching to a crash report. Returns the number of lines written. */
int copyRecentLogs(char* out, size_t cap);

/** Clear the crash fields of the breadcrumb after a crash has been reported. */
void clearBreadcrumbCrash();

}  // namespace telemetry
}  // namespace core

// ── Convenience macros ───────────────────────────────────────────────────────
#define LOGE(tag, ...) ::core::telemetry::log(::core::telemetry::Level::Error, (tag), __VA_ARGS__)
#define LOGW(tag, ...) ::core::telemetry::log(::core::telemetry::Level::Warn,  (tag), __VA_ARGS__)
#define LOGI(tag, ...) ::core::telemetry::log(::core::telemetry::Level::Info,  (tag), __VA_ARGS__)

#define LOGD(tag, ...) ::core::telemetry::log(::core::telemetry::Level::Debug, (tag), __VA_ARGS__)
#define LOGV(tag, ...) ::core::telemetry::log(::core::telemetry::Level::Verbose, (tag), __VA_ARGS__)
