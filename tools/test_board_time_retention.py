"""Compile the real core::time owner against a controlled RTC clock and SNTP.

Covers keeping the wall clock across deep sleep / soft reset without Wi-Fi,
rejecting it after power-on or when it never descended from a sync, local
timezone before any sync, the SNTP step report and the dev SNTP hold.
"""
from pathlib import Path
import subprocess, tempfile

ROOT = Path(__file__).resolve().parents[1]

STUBS = {
    'Arduino.h': r'''#pragma once
#include <cstdarg>
#include <cstdio>
#include <string>
#include <stdint.h>
extern std::string g_log;
struct FakeSerial { int printf(const char* f, ...) { char b[512]; va_list a; va_start(a, f);
  int n = vsnprintf(b, sizeof b, f, a); va_end(a); g_log += b; return n; }
  void println(const char* t) { g_log += t; g_log += "\n"; } };
extern FakeSerial Serial;
void configTzTime(const char* tz, const char*, const char*);
''',
    'esp_attr.h': '#pragma once\n#define RTC_NOINIT_ATTR\n',
    'esp_sntp.h': '#pragma once\n#include <sys/time.h>\ntypedef void (*sntp_sync_time_cb_t)(struct timeval*);\nvoid sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t);\n',
    'esp_system.h': '#pragma once\ntypedef enum { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC, ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP, ESP_RST_BROWNOUT } esp_reset_reason_t;\nesp_reset_reason_t esp_reset_reason();\n',
    'esp_newlib.h': '#pragma once\nvoid esp_sync_counters_rtc_and_frc(void);\n',
    'core/store.h': '#pragma once\n#include <stdint.h>\nnamespace core { namespace store { uint32_t getU32(const char*, const char*, uint32_t); bool putU32(const char*, const char*, uint32_t); } }\n',
    'esp_private/esp_clk.h': '#pragma once\n#include <stdint.h>\nuint32_t esp_clk_slowclk_cal_get(void);\n',
    'esp_timer.h': '#pragma once\n#include <stdint.h>\nint64_t esp_timer_get_time();\n',
    'freertos/FreeRTOS.h': '#pragma once\ntypedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\n#define portENTER_CRITICAL(x) (void)(x)\n#define portEXIT_CRITICAL(x) (void)(x)\n',
    'hal/hal_wifi.h': '#pragma once\nnamespace hal { namespace wifi { bool connected(); } }\n',
}

MAIN = r'''
#include "core/board_time.h"
#include "core/clock_retention.h"
#include <Arduino.h>
#include <esp_sntp.h>
#include <esp_system.h>
#include <esp_timer.h>
#include "hal/hal_wifi.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/time.h>
#include <time.h>

std::string g_log; FakeSerial Serial;
// Controlled RTC: wall clock in microseconds; monotonic esp_timer separately.
static int64_t g_wallUs = 0, g_monoUs = 0;
static esp_reset_reason_t g_reason = ESP_RST_POWERON;
static bool g_wifi = false; static int g_sntpStarts = 0; static sntp_sync_time_cb_t g_cb = nullptr;
extern "C" int settimeofday(const struct timeval* tv, const struct timezone*) {
  g_wallUs = (int64_t)tv->tv_sec * 1000000LL + tv->tv_usec; return 0; }
extern "C" int gettimeofday(struct timeval* tv, void*) {
  tv->tv_sec = (time_t)(g_wallUs / 1000000LL); tv->tv_usec = (suseconds_t)(g_wallUs % 1000000LL); return 0; }
extern "C" time_t time(time_t* out) { time_t t = (time_t)(g_wallUs / 1000000LL); if (out) *out = t; return t; }
esp_reset_reason_t esp_reset_reason() { return g_reason; }
int64_t esp_timer_get_time() { return g_monoUs; }
namespace hal { namespace wifi { bool connected() { return g_wifi; } } }
void sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t cb) { g_cb = cb; }
static uint32_t g_cal = 0;  // RC period IDF holds this boot (Q13.19 us); 0 = unknown
uint32_t esp_clk_slowclk_cal_get(void) { return g_cal; }
static int g_rtcSyncs = 0; static bool g_nvsHas = false, g_nvsOk = true; static uint32_t g_nvsPpm = 0; static int g_nvsWrites = 0;
namespace core { namespace store {
uint32_t getU32(const char* ns, const char* key, uint32_t def) { assert(!strcmp(ns, "clock") && !strcmp(key, "sleep_ppm")); return g_nvsHas ? g_nvsPpm : def; }
bool putU32(const char*, const char*, uint32_t v) { ++g_nvsWrites; if (!g_nvsOk) return false; g_nvsHas = true; g_nvsPpm = v; return true; }
} }
extern "C" void esp_sync_counters_rtc_and_frc(void) { ++g_rtcSyncs; }
void configTzTime(const char* tz, const char*, const char*) { ++g_sntpStarts; setenv("TZ", tz, 1); tzset(); }

static void elapse(int64_t us) { g_wallUs += us; g_monoUs += us; }
static void boot(esp_reset_reason_t reason) { g_reason = reason; g_monoUs = 0; g_log.clear(); core::time::init(); }
static void sync(int64_t epoch) {  // SNTP steps the clock then notifies.
  g_wallUs = epoch * 1000000LL; struct timeval tv{(time_t)epoch, 0}; assert(g_cb); g_cb(&tv); }
static std::string shown() { char b[8] = "x"; core::time::formatClockHHMM(b, sizeof b); return b; }
using core::time::Source;

int main() {
  {
    using namespace core::time;
    const int64_t sync = 1800000000;
    assert(clockDisplayFresh(sync, sync));
    assert(clockDisplayFresh(sync + kClockDisplayMaxAgeSeconds, sync));
    assert(!clockDisplayFresh(sync + kClockDisplayMaxAgeSeconds + 1, sync));
    assert(!clockDisplayFresh(sync, 0));
    assert(scheduledWakeStageSeconds(kScheduledWakeLeadSeconds) == 0);
    assert(scheduledWakeStageSeconds(-5) == 0);
    // Staging converges inside the lead window from any distance, and a clock
    // running 1% fast never makes a stage overshoot the alarm.
    for (int64_t start : {91LL, 3600LL, 8LL * 3600, 86400LL, 7LL * 86400}) {
      int64_t remaining = start; int stages = 0;
      while (int64_t stage = scheduledWakeStageSeconds(remaining)) {
        assert(stage > 0 && stage * 101 / 100 < remaining);
        remaining -= stage * 101 / 100; assert(++stages < 12);
      }
    }
  }

  namespace ct = core::time;
  // --- pure policy -----------------------------------------------------------
  ct::RetainedClock r{}; memset(&r, 0xA5, sizeof r);
  assert(!ct::retainedClockSealed(r));
  assert(!ct::retainedClockTrusted(r, false, 1800000000));
  ct::recordTrustedSync(r, 1800000000, 1234);
  assert(r.syncCount == 1 && r.sleepPpm == 1234 && r.sleepEnterUs == 0 && ct::retainedClockSealed(r));
  assert(ct::retainedClockTrusted(r, false, 1800000100));      // deep sleep / soft reset
  assert(!ct::retainedClockTrusted(r, true, 1800000100));      // power-on: RTC restarted
  assert(!ct::retainedClockTrusted(r, false, 5));              // RTC domain reset to 1970
  assert(ct::retainedClockTrusted(r, false, 1800000000 - 60)); // small backwards slack
  assert(!ct::retainedClockTrusted(r, false, 1800000000 - 3600)); // went backwards
  r.sleepPpm = 77; ct::sealRetainedClock(r);
  ct::recordTrustedSync(r, 1800000500, 0); assert(r.syncCount == 2 && r.sleepPpm == 77);  // keeps rate
  ct::RetainedClock t = r; t.lastSyncEpoch += 1; assert(!ct::retainedClockSealed(t));  // checksum
  t = r; t.sleepPpm += 1; assert(!ct::retainedClockSealed(t));
  // 1% fast RC: 1010 s measured is 1000 s real.
  assert(ct::sleepCorrectionUs(1010000000LL, 10000) == 10000000LL);
  assert(ct::sleepCorrectionUs(-5, 10000) == 0 && ct::sleepCorrectionUs(1000, 0) == 0);
  assert(ct::learnSleepPpm(0, -10000000LL, 1010000000LL) == 10000);
  assert(ct::learnSleepPpm(10000, 0, 1010000000LL) == 10000);
  assert(ct::learnSleepPpm(5000, 1000000000LL - (1010000000LL - ct::sleepCorrectionUs(1010000000LL, 5000)), 1010000000LL) == 10000);  // refines a partial estimate
  assert(ct::learnSleepPpm(500, -1000000LL, 300000000LL) == 500);     // too short to learn
  assert(ct::learnSleepPpm(-3000, 0, 1000000000LL) == -3000);         // already right
  assert(ct::learnSleepPpm(0, -900000000LL, 1000000000LL) == ct::kMaxSleepPpm);  // bounded

  // --- cold power-on, no Wi-Fi: blank clock, never a stale value -------------
  g_wallUs = 1234567LL * 1000000LL;  // garbage RTC value
  boot(ESP_RST_POWERON);
  assert(g_wallUs == 0 && !ct::isSynced() && ct::source() == Source::Unset && shown().empty());
  assert(ct::timezoneConfigured());
  ct::service(0); assert(g_sntpStarts == 0);  // no Wi-Fi: no SNTP

  // --- Wi-Fi up, SNTP syncs: clock valid, record sealed ----------------------
  g_wifi = true; ct::service(0); assert(g_sntpStarts == 1);
  ct::service(0); assert(g_sntpStarts == 1);  // started once
  const int64_t synced = 1793736000;  // 2026-11-03 20:00:00 UTC = 12:00 PST
  elapse(3000000); sync(synced); ct::service(0);
  assert(g_log.find("set_from_unset") != std::string::npos);
  assert(ct::source() == Source::Network && ct::isSynced() && shown() == "12:00");
  assert(ct::lastNetworkSyncEpoch() == synced);

  // --- deep sleep for an hour with Wi-Fi off: RTC keeps counting -------------
  g_wifi = false; elapse(3600LL * 1000000LL);
  unsetenv("TZ"); tzset();  // a new boot has no TZ until init() applies it
  boot(ESP_RST_DEEPSLEEP);
  assert(ct::source() == Source::Retained && ct::isSynced());
  assert(g_wallUs / 1000000LL == synced + 3600);
  assert(shown() == "13:00");  // local time (PST) without any network
  assert(g_log.find("source=retained") != std::string::npos);
  ct::service(0); assert(g_sntpStarts == 1);  // still no Wi-Fi, no SNTP
  ct::prepareForDeepSleep(); assert(g_rtcSyncs == 1);

  // --- panic / watchdog / software reset also keep it -------------------------
  elapse(10 * 1000000LL); boot(ESP_RST_PANIC); assert(ct::source() == Source::Retained);
  elapse(10 * 1000000LL); boot(ESP_RST_SW); assert(ct::source() == Source::Retained);

  // --- next sync reports the drift step the RTC accumulated ------------------
  g_wifi = true; ct::service(0); assert(g_sntpStarts == 2);
  elapse(1000000); const int64_t before = g_wallUs;
  g_wallUs = before + 1500000; struct timeval tv{(time_t)(g_wallUs / 1000000LL), (suseconds_t)(g_wallUs % 1000000LL)};
  g_cb(&tv); ct::service(0);
  assert(g_log.find("step=1500 ms") != std::string::npos);
  assert(ct::source() == Source::Network);

  // --- RTC domain reset (e.g. brownout that cleared the timer) ---------------
  g_wifi = false; g_wallUs = 42LL * 1000000LL; boot(ESP_RST_BROWNOUT);
  assert(ct::source() == Source::Unset && g_wallUs == 0 && shown().empty());

  // --- power-on with an otherwise plausible clock is still rejected ----------
  g_wifi = true; boot(ESP_RST_POWERON); ct::service(0); sync(synced); ct::service(0);
  g_wifi = false; elapse(5 * 1000000LL); boot(ESP_RST_POWERON);
  assert(ct::source() == Source::Unset && !ct::isSynced());
  // ...and the record was invalidated, so a later soft reset does not revive it.
  g_wallUs = (synced + 100) * 1000000LL; boot(ESP_RST_SW);
  assert(ct::source() == Source::Unset && !ct::isSynced());

  // --- calibration rescale (pure) -----------------------------------------
  assert(ct::rescaleSleepUs(1000000000LL, 0, 3500000) == 1000000000LL);
  assert(ct::rescaleSleepUs(1000000000LL, 3600000, 3564000) == 990000000LL);
  assert(ct::rescaleSleepUs(-1, 1, 2) == -1);

  // --- deep-sleep RC drift: learn the rate from a sync, then correct on wake --
  // Real time advances 1000 s while the RC-counted clock advances 1010 s (+1%).
  const int64_t sleepReal = 1000LL * 1000000LL, sleepRc = 1010LL * 1000000LL;
  int64_t realUs = (synced + 20000) * 1000000LL;
  g_wifi = true; g_wallUs = realUs; boot(ESP_RST_SW); ct::service(0); sync(realUs / 1000000LL); ct::service(0);
  assert(ct::sleepDriftPpm() == 0);
  g_wifi = false; ct::prepareForDeepSleep(); g_wallUs += sleepRc; realUs += sleepReal; boot(ESP_RST_DEEPSLEEP);
  assert(ct::source() == Source::Retained && g_wallUs - realUs == 10000000LL);  // uncorrected
  assert(g_log.find("corrected_ms=0") != std::string::npos);
  // Wi-Fi returns: the sync step (-10 s) teaches the rate and saves it to NVS.
  g_wifi = true; ct::service(0); elapse(1000000); realUs += 1000000;
  g_wallUs = realUs; { struct timeval tv{(time_t)(realUs / 1000000LL), 0}; g_cb(&tv); } ct::service(0);
  assert(g_log.find("step=-10000 ms") != std::string::npos);
  assert(ct::sleepDriftPpm() == 10000 && g_nvsHas && (int32_t)g_nvsPpm == 10000);
  int writes = g_nvsWrites;
  // Next sleep is corrected on wake, with no network.
  g_wifi = false; ct::prepareForDeepSleep(); g_wallUs += sleepRc; realUs += sleepReal; boot(ESP_RST_DEEPSLEEP);
  { const int64_t err = g_wallUs - realUs; assert(err > -2000 && err < 2000); }
  assert(g_log.find("corrected_ms=1000") != std::string::npos);  // 10000 ms, prefix-matched
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  // Trial: the wake calibration is logged but no longer rescales the interval.
  g_wifi = true; ct::setSleepDriftPpmForTest(0); g_wifi = false;
  g_cal = 3600000; ct::prepareForDeepSleep();
  g_wallUs += sleepRc; realUs += sleepReal;  // 1010 s of ticks at the pre-sleep period
  g_cal = 3564356;                            // wake: period 1% shorter (RC 1% faster)
  boot(ESP_RST_DEEPSLEEP);
  { const int64_t err = g_wallUs - realUs - (sleepRc - sleepReal); assert(err > -2000 && err < 2000); }
  g_wallUs -= sleepRc - sleepReal;  // Later checks start from an aligned clock.
  assert(g_log.find("rc_hz=") != std::string::npos);
  g_cal = 0; ct::setSleepDriftPpmForTest(10000);
  writes = g_nvsWrites;
#endif
  // A soft reset after waking does not apply the correction twice.
  const int64_t beforeReset = g_wallUs; boot(ESP_RST_SW); assert(g_wallUs == beforeReset);
  // An accurate sync (no step) keeps the rate and does not rewrite NVS.
  g_wifi = true; ct::service(0); g_wallUs = realUs; { struct timeval tv{(time_t)(realUs / 1000000LL), (suseconds_t)(realUs % 1000000LL)}; g_cb(&tv); } ct::service(0);
  assert(ct::sleepDriftPpm() == 10000 && g_nvsWrites == writes);
  // Power loss clears the RTC record; the learned rate comes back from NVS on the next sync.
  g_wifi = true; boot(ESP_RST_POWERON); assert(ct::sleepDriftPpm() == 10000);
  ct::service(0); realUs += 5000000; sync(realUs / 1000000LL); ct::service(0);
  g_wifi = false; ct::prepareForDeepSleep(); g_wallUs += sleepRc; realUs += sleepReal; boot(ESP_RST_DEEPSLEEP);
  { const int64_t err = g_wallUs - realUs; assert(err > -2000 && err < 2000); }
  // Failed NVS save keeps the refined rate in the RTC copy and warns.
  g_nvsOk = false; g_wifi = true; ct::service(0);
  realUs = g_wallUs - 1000000LL; g_wallUs = realUs;  // SNTP steps the clock back 1 s
  { struct timeval tv{(time_t)(realUs / 1000000LL), (suseconds_t)(realUs % 1000000LL)}; g_cb(&tv); } ct::service(0);
  assert(g_log.find("NVS save failed") != std::string::npos);
  assert(ct::sleepDriftPpm() > 10000 && (int32_t)g_nvsPpm == 10000);
  g_nvsOk = true;

#ifdef ORIGAMI_DEV_SERIAL_INPUT
  ct::setDevSntpHold(true); assert(ct::devSntpHold());
  g_wifi = true; const int starts = g_sntpStarts; boot(ESP_RST_DEEPSLEEP); ct::service(0);
  assert(g_sntpStarts == starts);
  ct::setDevSntpHold(false); ct::service(0); assert(g_sntpStarts == starts + 1);
#endif
  return 0;
}
'''

for dev in (False, True):
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        for name, text in STUBS.items():
            p = root / name; p.parent.mkdir(parents=True, exist_ok=True); p.write_text(text)
        (root / 'main.cpp').write_text(MAIN)
        exe = root / 'test'
        cmd = ['c++', '-std=c++17', '-Wall', '-Wextra', '-fsanitize=address,undefined',
               '-I', str(root), '-I', str(ROOT / 'include'),
               str(root / 'main.cpp'), str(ROOT / 'src/core/board_time.cpp'), '-o', str(exe)]
        if dev: cmd.insert(2, '-DORIGAMI_DEV_SERIAL_INPUT')
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True)
print('board time retention: ok')
