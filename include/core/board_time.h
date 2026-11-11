#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

namespace core {
namespace time {

/**
 * Decide at boot whether the wall clock the RTC timer carried into this boot is
 * trustworthy. Call ONCE at boot, before anything reads the time.
 *
 * The ESP32-S3 keeps system time running in its RTC timer through deep sleep,
 * light sleep and software/panic/watchdog resets (slow clock: internal ~136 kHz
 * RC; no 32 kHz crystal on this board). A record in RTC_NOINIT memory, sealed only by
 * a successful SNTP sync, marks that time as descending from a real sync. If the
 * record is intact and the clock is plausible the time is kept (Source::Retained,
 * no Wi-Fi needed); otherwise (power-on, RTC domain reset, never synced) the
 * clock is forced to "unset" so a stale value can never masquerade as real.
 * Also applies the product timezone so local time is correct before any sync.
 * UI task, non-blocking.
 */
void init();

/**
 * Single source of truth for "is the wall clock trustworthy?". True after a
 * network sync this boot, or when init() kept a clock the RTC carried across
 * sleep/reset from an earlier sync. Stays true if WiFi later drops. Gate every
 * time-driven feature (clock display, alarms) on this.
 */
bool isSynced();

enum class Source : uint8_t {
  Unset,     // no trusted time yet (cold power-on and no SNTP sync)
  Retained,  // carried by the RTC timer across sleep/reset from an earlier sync
  Network,   // SNTP synced during this boot
};

/** Where the current wall clock came from (diagnostics; any task). */
Source source();
const char* sourceName(Source value);

/** Epoch of the most recent trusted sync recorded in RTC memory, or 0. */
int64_t lastNetworkSyncEpoch();

/** Learned deep-sleep RC rate error applied on wake (ppm, + = RC fast). */
int32_t sleepDriftPpm();

/**
 * Call from main loop (any task). Starts SNTP once when WiFi is up and logs the
 * step each sync applies ("[time] sntp sync"). After a sync that follows at
 * least 10 minutes of deep sleep it refines the learned sleep rate and saves it
 * to NVS (rare; one short flash write), otherwise it does not block.
 */
void service(uint32_t millis_now);

/**
 * Call immediately before esp_deep_sleep_start() (any task, non-blocking). Aligns
 * the RTC timer base with the accurate running clock and records the sleep start
 * so init() on wake can remove the RC slow clock's learned rate error from the
 * slept interval (measured ~+0.8..0.9% uncorrected on rev-4 hardware).
 */
void prepareForDeepSleep();

/** Wall clock "HH:MM" once trusted (see isSynced); empty string (blank) until then — never a wrong time. */
void formatClockHHMM(char* buf, size_t buf_len);

/** True after init() applied the product timezone (needed before mktime/localtime are local). */
bool timezoneConfigured();

/**
 * Encode a local calendar date as YYYYMMDD (e.g. 20260520).
 * Returns 0 if the timezone has not been configured yet (clock is untrustworthy).
 * Call timezoneConfigured() first if you need to distinguish "not ready" from a real date.
 * This is a pure computation — no side effects.
 */
uint32_t localDayKey();

#ifdef ORIGAMI_DEV_SERIAL_INPUT
/** Dev builds: keep SNTP from starting (survives deep sleep/soft reset; not
 *  power-on) so RTC-only timekeeping can be measured. Does not stop an SNTP
 *  client already running this boot. */
void setDevSntpHold(bool hold);
/** Dev builds: override the learned sleep rate (RTC record and NVS). */
bool setSleepDriftPpmForTest(int32_t ppm);
bool devSntpHold();
#endif

}  // namespace time
}  // namespace core
