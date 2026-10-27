#include "core/board_time.h"

#include <Arduino.h>
#include <cstdio>
#include <atomic>
#include <sys/time.h>
#include <time.h>

#include "hal/hal_wifi.h"

namespace core {
namespace time {

// Epoch below which the wall clock is treated as unset (~2023-11-14). The ESP32
// boots its RTC at 1970 and init() re-zeroes it every boot, so nothing but a
// successful SNTP sync ever pushes the clock past this — making "> threshold" a
// reliable "synced this session" test that defeats stale RTC carry-over.
static constexpr time_t kSyncedEpochThreshold = (time_t)1700000000;

static std::atomic<bool> s_sntp_started{false};
static bool s_tz_configured = false;

void init() {
  // Wipe any time the RTC carried across a soft reboot back to "unset" so a stale
  // value can never look like a real clock. Only SNTP (service(), once WiFi is up)
  // may move it forward from here.
  struct timeval tv;
  tv.tv_sec  = 0;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
  s_sntp_started   = false;
  s_tz_configured  = false;
}

bool isSynced() { return ::time(nullptr) > kSyncedEpochThreshold; }

void service(uint32_t millis_now) {
  (void)millis_now;
  // Start SNTP on the first connected service tick, not an arbitrary 10s
  // polling boundary. Only one caller may start it; SNTP owns its own retries.
  if (!hal::wifi::connected() || s_sntp_started.exchange(true)) return;
  configTzTime("PST8PDT,M3.2.0,M11.1.0", "pool.ntp.org", "time.nist.gov");
  s_tz_configured = true;
}

bool timezoneConfigured() { return s_tz_configured; }

uint32_t localDayKey() {
  if (!s_tz_configured || !isSynced()) return 0;
  time_t now = ::time(nullptr);
  struct tm t;
  ::localtime_r(&now, &t);
  return (uint32_t)((t.tm_year + 1900) * 10000 + (t.tm_mon + 1) * 100 + t.tm_mday);
}

void formatClockHHMM(char* buf, size_t buf_len) {
  if (!buf || buf_len < 6) {
    return;
  }
  // Never render a time we don't trust. Until SNTP has synced over WiFi the clock
  // is blank (empty string) — not "--:--" and never a stale RTC value — so a wrong
  // time can't appear after a reboot/power-cycle. Once synced it stays shown.
  const time_t now = ::time(nullptr);
  if (now <= kSyncedEpochThreshold) {
    buf[0] = '\0';
    return;
  }
  // Arduino getLocalTime(..., 0) can skip its loop entirely if millis()
  // advances between its start/deadline reads. That makes a valid clock flash
  // "--:--" for a frame. Convert the same trusted epoch snapshot directly;
  // there is no SNTP wait or millisecond deadline in this rendering path.
  struct tm tinfo;
  if (::localtime_r(&now, &tinfo)) {
    snprintf(buf, buf_len, "%02d:%02d", tinfo.tm_hour, tinfo.tm_min);
  } else {
    buf[0] = '\0';
  }
}

}  // namespace time
}  // namespace core
