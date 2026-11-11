#include "core/board_time.h"

#include <Arduino.h>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <esp_attr.h>
extern "C" {
#include <esp_newlib.h>  // esp_sync_counters_rtc_and_frc (no C++ guard)
}
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_private/esp_clk.h>  // esp_clk_slowclk_cal_get: RC period IDF uses
#if defined(ORIGAMI_RTC_SLOW_8MD256)
#include <soc/rtc.h>
#endif
#include <freertos/FreeRTOS.h>
#include <sys/time.h>
#include <time.h>

#include "core/clock_retention.h"
#include "core/store.h"
#include "hal/hal_wifi.h"

namespace core {
namespace time {

static constexpr const char* kTimezone = "PST8PDT,M3.2.0,M11.1.0";
static constexpr time_t kSyncedEpochThreshold = (time_t)kValidEpochThreshold;

// Survives deep sleep and software/panic/WDT resets, exactly like the RTC timer
// that carries system time; garbage after power-on (sealed by magic+checksum).
RTC_NOINIT_ATTR static RetainedClock s_retained;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
static constexpr uint32_t kDevSntpHoldMagic = 0x484f4c44u;  // "HOLD"
RTC_NOINIT_ATTR static uint32_t s_devSntpHold;
#endif

static std::atomic<bool> s_sntp_started{false};
static std::atomic<uint8_t> s_source{(uint8_t)Source::Unset};
static std::atomic<uint32_t> s_syncs{0};  // trusted syncs this boot (SNTP callback)
static bool s_tz_configured = false;
// Record mutations come from the SNTP callback (tcpip task), service() and the
// UI-task boot/sleep hooks; keep each read-modify-seal coherent.
static portMUX_TYPE s_recordLock = portMUX_INITIALIZER_UNLOCKED;
// Learned deep-sleep rate, mirrored in NVS so it survives power loss (which
// clears the RTC record). Missing -> 0 (uncorrected until the first learn).
static constexpr const char* kStoreNs = "clock";
static constexpr const char* kStorePpmKey = "sleep_ppm";
static int32_t s_freshPpm = 0;

// Last wall/monotonic sample, used to report the step each SNTP sync applies
// (i.e. how far the free-running RTC clock had drifted).
static portMUX_TYPE s_sampleLock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_sampleWallUs = 0;
static int64_t s_sampleMonoUs = 0;
static uint32_t s_sampleSyncs = 0;

// RC slow clock frequency (Hz) from a Q13.19 period in microseconds.
#if defined(ORIGAMI_RTC_SLOW_8MD256)
static constexpr uint32_t kTrialCalCycles = 2048;
#endif

#if defined(ORIGAMI_RTC_SLOW_8MD256)
// Trial: boot re-selects the sdkconfig RC source, so select 8MD256 again here.
// esp_clk_slowclk_cal_set banks elapsed ticks at the old period before writing
// the new one, and esp_sleep_start keeps the 8M oscillator on for this source.
static void selectTrialSlowClock() {
  rtc_clk_8m_enable(true, true);
  rtc_clk_slow_freq_set(RTC_SLOW_FREQ_8MD256);
  const uint32_t cal = rtc_clk_cal(RTC_CAL_RTC_MUX, kTrialCalCycles);
  if (cal) esp_clk_slowclk_cal_set(cal);
}
#endif

static double calHz(uint32_t cal) { return cal ? 1e6 * 524288.0 / (double)cal : 0.0; }

static int64_t wallUs() {
  struct timeval tv;
  ::gettimeofday(&tv, nullptr);
  return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

// lwIP/tcpip task. Only seals the RTC record and counts; logging happens in service().
static void onSntpSync(struct timeval* tv) {
  const int64_t epoch = tv ? (int64_t)tv->tv_sec : (int64_t)::time(nullptr);
  if (epoch <= kValidEpochThreshold) return;
  portENTER_CRITICAL(&s_recordLock);
  recordTrustedSync(s_retained, epoch, s_freshPpm);
  portEXIT_CRITICAL(&s_recordLock);
  s_source.store((uint8_t)Source::Network);
  s_syncs.fetch_add(1);
}

void init() {
#if defined(ORIGAMI_RTC_SLOW_8MD256)
  selectTrialSlowClock();
#endif
  // The timezone is a fixed product constant; apply it before anything formats a
  // local time so a clock carried across sleep is shown in local time, not UTC.
  setenv("TZ", kTimezone, 1);
  tzset();
  s_tz_configured = true;
  s_sntp_started = false;
  s_syncs = 0;

  const esp_reset_reason_t reason = esp_reset_reason();
  const time_t now = ::time(nullptr);
  const bool trusted = retainedClockTrusted(s_retained, reason == ESP_RST_POWERON, (int64_t)now);
  int64_t correctedMs = 0, sleptMs = 0;
  double preHz = 0, wakeHz = 0;
  if (trusted) {
    s_source = (uint8_t)Source::Retained;
    if (s_retained.sleepEnterUs > 0) {
      // Waking from SW1 deep sleep: remove the RC slow clock's learned rate error
      // from the RTC-measured interval (also covers the short boot before here).
      const int64_t nowUs = wallUs();
      const int64_t raw = nowUs - s_retained.sleepEnterUs;
      if (reason == ESP_RST_DEEPSLEEP && raw > 0) {
        // IDF converted the slept ticks with the pre-sleep calibration; use the
        // one this boot just measured on the cold chip, then the learned residual.
        const uint32_t wakeCal = esp_clk_slowclk_cal_get();
        // Trial: no rescale. It worsened 300 s RMS on device; wakeCal is logged only.
        const int64_t measured = raw;
        const int64_t correction = (raw - measured) + sleepCorrectionUs(measured, s_retained.sleepPpm);
        const int64_t fixedUs = nowUs - correction;
        preHz = calHz(s_retained.sleepCal);
        wakeHz = calHz(wakeCal);
        struct timeval tv{(time_t)(fixedUs / 1000000LL), (suseconds_t)(fixedUs % 1000000LL)};
        settimeofday(&tv, nullptr);
        s_retained.sleepUsSinceSync += measured;
        correctedMs = correction / 1000;
        sleptMs = raw / 1000;
      }
      s_retained.sleepEnterUs = 0;
      sealRetainedClock(s_retained);
    }
  } else {
    // The RTC did not carry a time descending from a trusted sync (power-on,
    // RTC domain reset, or no sync since power-on). Force it to "unset" so a
    // stale value can never look real; only SNTP may set it from here.
    struct timeval tv{0, 0};
    settimeofday(&tv, nullptr);
    s_retained.magic = 0;
    s_source = (uint8_t)Source::Unset;
    s_freshPpm = (int32_t)core::store::getU32(kStoreNs, kStorePpmKey, 0u);
    if (s_freshPpm > kMaxSleepPpm || s_freshPpm < -kMaxSleepPpm) s_freshPpm = 0;
  }
  {
    portENTER_CRITICAL(&s_sampleLock);
    s_sampleWallUs = wallUs();
    s_sampleMonoUs = esp_timer_get_time();
    s_sampleSyncs = 0;
    portEXIT_CRITICAL(&s_sampleLock);
  }
#if defined(ORIGAMI_RTC_SLOW_8MD256)
  Serial.printf("[time] slow_clk src=%d hz=%.0f\n", (int)rtc_clk_slow_freq_get(),
                calHz(esp_clk_slowclk_cal_get()));
#endif
  Serial.printf("[time] boot reset=%d source=%s epoch=%ld last_sync=%lld syncs=%lu "
                "slept_ms=%lld corrected_ms=%lld sleep_ppm=%ld rc_hz=%.0f->%.0f\n",
                (int)reason, sourceName(source()), (long)::time(nullptr),
                trusted ? (long long)s_retained.lastSyncEpoch : 0LL,
                trusted ? (unsigned long)s_retained.syncCount : 0UL, (long long)sleptMs,
                (long long)correctedMs, (long)(trusted ? s_retained.sleepPpm : s_freshPpm),
                preHz, wakeHz);
}

bool isSynced() { return ::time(nullptr) > kSyncedEpochThreshold; }

Source source() { return (Source)s_source.load(); }

const char* sourceName(Source value) {
  switch (value) {
    case Source::Retained: return "retained";
    case Source::Network:  return "network";
    default:               return "unset";
  }
}

int32_t sleepDriftPpm() {
  return retainedClockSealed(s_retained) ? s_retained.sleepPpm : s_freshPpm;
}

int64_t lastNetworkSyncEpoch() {
  return retainedClockSealed(s_retained) ? s_retained.lastSyncEpoch : 0;
}

static void reportSyncSteps() {
  const uint32_t syncs = s_syncs.load();
  const int64_t mono = esp_timer_get_time();
  const int64_t wall = wallUs();
  portENTER_CRITICAL(&s_sampleLock);
  const int64_t prevWall = s_sampleWallUs, prevMono = s_sampleMonoUs;
  const uint32_t prevSyncs = s_sampleSyncs;
  s_sampleWallUs = wall;
  s_sampleMonoUs = mono;
  s_sampleSyncs = syncs;
  portEXIT_CRITICAL(&s_sampleLock);
  if (syncs == prevSyncs) return;
  // Positive step: the clock was behind real time before this sync.
  const int64_t stepUs = (wall - prevWall) - (mono - prevMono);
  const bool wasValid = prevWall / 1000000LL > kValidEpochThreshold;
  int32_t oldPpm = 0, newPpm = 0;
  int64_t sleptUs = 0;
  portENTER_CRITICAL(&s_recordLock);
  const bool sealed = retainedClockSealed(s_retained);
  if (sealed) {
    oldPpm = s_retained.sleepPpm;
    sleptUs = s_retained.sleepUsSinceSync;
    newPpm = wasValid ? learnSleepPpm(oldPpm, stepUs, sleptUs) : oldPpm;
    s_retained.sleepPpm = newPpm;
    s_retained.sleepUsSinceSync = 0;  // the next interval starts at this sync
    sealRetainedClock(s_retained);
  }
  portEXIT_CRITICAL(&s_recordLock);
  Serial.printf("[time] sntp sync #%lu epoch=%ld %s=%lld ms slept_since_sync_s=%lld "
                "sleep_ppm=%ld->%ld\n", (unsigned long)syncs, (long)(wall / 1000000LL),
                wasValid ? "step" : "set_from_unset", wasValid ? (long long)(stepUs / 1000) : 0LL,
                (long long)(sleptUs / 1000000LL), (long)oldPpm, (long)newPpm);
  if (sealed && newPpm != oldPpm) {
    s_freshPpm = newPpm;
    if (!core::store::putU32(kStoreNs, kStorePpmKey, (uint32_t)newPpm))
      Serial.println("[time] WARN: sleep_ppm NVS save failed; RTC copy remains");
  }
}

void service(uint32_t millis_now) {
  (void)millis_now;
  reportSyncSteps();
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  if (s_devSntpHold == kDevSntpHoldMagic) return;
#endif
  // Start SNTP on the first connected service tick, not an arbitrary 10s
  // polling boundary. Only one caller may start it; SNTP owns its own retries.
  if (!hal::wifi::connected() || s_sntp_started.exchange(true)) return;
  sntp_set_time_sync_notification_cb(onSntpSync);
  configTzTime(kTimezone, "pool.ntp.org", "time.nist.gov");
}

void prepareForDeepSleep() {
  // With both the esp_timer (XTAL) and RTC timer backing system time, align the
  // RTC-derived base with the accurate esp_timer clock so the wake-time restore
  // carries only the sleep interval's RC drift, not the whole awake period's.
  esp_sync_counters_rtc_and_frc();
  const uint32_t sleepCal = esp_clk_slowclk_cal_get();
  const int64_t enterUs = wallUs();  // outside the spinlock: gettimeofday locks
  portENTER_CRITICAL(&s_recordLock);
  const bool sealed = retainedClockSealed(s_retained);
  if (sealed) {
    s_retained.sleepEnterUs = enterUs;
    s_retained.sleepCal = sleepCal;
    sealRetainedClock(s_retained);
  }
  portEXIT_CRITICAL(&s_recordLock);
  Serial.printf("[time] deep sleep source=%s epoch=%ld sleep_ppm=%ld\n", sourceName(source()),
                (long)::time(nullptr), (long)(sealed ? s_retained.sleepPpm : 0));
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
  // Never render a time we don't trust. Until the clock is valid (network sync
  // this power-on, carried by the RTC across sleep/reset) it is blank (empty
  // string) — not "--:--" and never an unsynced value.
  const time_t now = ::time(nullptr);
  if (now <= kSyncedEpochThreshold || !clockDisplayFresh((int64_t)now, lastNetworkSyncEpoch())) {
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

#ifdef ORIGAMI_DEV_SERIAL_INPUT
void setDevSntpHold(bool hold) { s_devSntpHold = hold ? kDevSntpHoldMagic : 0u; }
bool setSleepDriftPpmForTest(int32_t ppm) {
  portENTER_CRITICAL(&s_recordLock);
  const bool sealed = retainedClockSealed(s_retained);
  if (sealed) {
    s_retained.sleepPpm = ppm;
    sealRetainedClock(s_retained);
  }
  portEXIT_CRITICAL(&s_recordLock);
  s_freshPpm = ppm;
  return core::store::putU32(kStoreNs, kStorePpmKey, (uint32_t)ppm) && sealed;
}
bool devSntpHold() { return s_devSntpHold == kDevSntpHoldMagic; }
#endif

}  // namespace time
}  // namespace core
