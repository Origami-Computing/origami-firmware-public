#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

namespace core {
namespace time {

/**
 * Force the wall clock into an explicitly-unset state. Call ONCE at boot, before
 * anything reads the time. The ESP32's RTC keeps counting a previously-synced
 * value across a soft reboot (crash/panic/OTA), so without this a stale time
 * would masquerade as real — showing a wrong clock and arming alarms off it.
 * After init() only an SNTP sync (over WiFi) can make the clock valid again.
 */
void init();

/**
 * Single source of truth for "is the wall clock trustworthy?". True only once
 * SNTP has set a sane time this session. Because init() zeroes the clock at boot,
 * the only thing that flips this to true is a real network sync. Once true it
 * stays true even if WiFi later drops (the RTC keeps counting from the synced
 * value). Gate every time-driven feature (clock display, alarms) on this.
 */
bool isSynced();

/** Call from main loop; starts SNTP once when WiFi is up. */
void service(uint32_t millis_now);

/** Wall clock "HH:MM" once synced; empty string (blank) until then — never a wrong time. */
void formatClockHHMM(char* buf, size_t buf_len);

/** True after `configTzTime` has run (needed before mktime/getLocalTime are consistent in local TZ). */
bool timezoneConfigured();

/**
 * Encode a local calendar date as YYYYMMDD (e.g. 20260520).
 * Returns 0 if the timezone has not been configured yet (clock is untrustworthy).
 * Call timezoneConfigured() first if you need to distinguish "not ready" from a real date.
 * This is a pure computation — no side effects.
 */
uint32_t localDayKey();

}  // namespace time
}  // namespace core
