#include "core/telemetry.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "core/telemetry_queue.h"

namespace core {
namespace telemetry {

namespace {

// ── RAM ring buffer ──────────────────────────────────────────────────────────
constexpr int kRingLines = 16;
constexpr int kLineCap   = 96;

// Allocated in PSRAM (see telemetry_queue.cpp for why internal DRAM must be left
// free for WiFi re-init). Row pointer: s_ring[i] is a char[kLineCap].
char            (*s_ring)[kLineCap] = nullptr;
int               s_ringHead  = 0;   // next write slot
int               s_ringCount = 0;   // valid lines (<= kRingLines)
SemaphoreHandle_t s_mutex     = nullptr;

// ── RTC breadcrumb (survives a soft reset; NOT cleared on panic/WDT) ─────────
constexpr uint32_t kBreadcrumbMagic = 0x4B414D49;  // 'KAMI'
struct Breadcrumb {
  uint32_t magic;
  char     lastTag[16];
  char     hungTask[16];
};
RTC_NOINIT_ATTR Breadcrumb s_crumb;

bool lock() { return s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(50)) == pdTRUE; }
void unlock() { if (s_mutex) xSemaphoreGive(s_mutex); }

const char* levelStr(Level l) {
  switch (l) {
    case Level::Verbose: return "VERBOSE";
    case Level::Debug: return "DEBUG";
    case Level::Error: return "ERROR";
    case Level::Warn:  return "WARN";
    default:           return "INFO";
  }
}

// Wall clock is only trustworthy once SNTP has set it (epoch jumps past ~2023).
// Before that — e.g. an error logged at boot before WiFi/SNTP, or while a device
// never connects — time(nullptr) returns seconds-since-boot (~1970). Stamping
// that as a real timestamp produces logs dated 1970/wrong-year, so emit 0 and let
// the server's authoritative created_at stand in. Mirrors automation::wallClockSynced().
uint32_t syncedEpochOrZero() {
  time_t t = time(nullptr);
  return (t > (time_t)1700000000) ? (uint32_t)t : 0u;
}

}  // namespace

void init() {
  if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
  // Ring buffer in PSRAM, not internal DRAM (leave internal heap for WiFi).
  if (!s_ring) {
    s_ring = (char(*)[kLineCap])heap_caps_malloc(
        sizeof(char) * kRingLines * kLineCap, MALLOC_CAP_SPIRAM);
  }
  // First ever boot (cold power-on): RTC RAM is garbage — stamp the magic so the
  // breadcrumb is well-defined. On a soft/panic/WDT reset the magic persists and
  // we leave the recorded fields intact for crash_report to read.
  if (s_crumb.magic != kBreadcrumbMagic) {
    s_crumb.magic = kBreadcrumbMagic;
    s_crumb.lastTag[0]  = '\0';
    s_crumb.hungTask[0] = '\0';
  }
}

void log(Level level, const char* tag, const char* fmt, ...) {
  if (!tag) tag = "?";
  char msg[kLineCap];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  // 1) Serial, preserving the existing "[tag] message" convention.
  Serial.printf("[%s] %s\n", tag, msg);

  // 2) RAM ring + breadcrumb tag.
  if (lock()) {
    if (s_ring) {
      snprintf(s_ring[s_ringHead], kLineCap, "[%s] %s", tag, msg);
      s_ringHead = (s_ringHead + 1) % kRingLines;
      if (s_ringCount < kRingLines) s_ringCount++;
    }
    strlcpy(s_crumb.lastTag, tag, sizeof(s_crumb.lastTag));
    unlock();
  }

  // 3) Forward all levels. Routine diagnostics are RAM-only and bounded.
  JsonDocument doc;
  doc["ts"]    = syncedEpochOrZero();  // 0 when clock unsynced; server created_at is authoritative
  doc["level"] = levelStr(level);
  doc["tag"]   = tag;
  doc["msg"]   = msg;
  char json[telemetry_queue::kItemJsonCap];
  serializeJson(doc, json, sizeof(json));
  if (measureJson(doc) >= sizeof(json)) return;
  const bool durable = level == Level::Error || level == Level::Warn;
  telemetry_queue::enqueue(durable ? telemetry_queue::KIND_LOG : telemetry_queue::KIND_VERBOSE, json);
}

void setBreadcrumbHungTask(const char* taskName) {
  // Called from the TWDT callback just before reset — keep it allocation-free
  // and lock-free (the system is already wedged).
  if (s_crumb.magic != kBreadcrumbMagic) s_crumb.magic = kBreadcrumbMagic;
  strlcpy(s_crumb.hungTask, taskName ? taskName : "?", sizeof(s_crumb.hungTask));
}

bool breadcrumbValid()        { return s_crumb.magic == kBreadcrumbMagic; }
const char* breadcrumbLastTag()  { return breadcrumbValid() ? s_crumb.lastTag  : ""; }
const char* breadcrumbHungTask() { return breadcrumbValid() ? s_crumb.hungTask : ""; }

int copyRecentLogs(char* out, size_t cap) {
  if (!out || cap == 0) return 0;
  out[0] = '\0';
  if (!s_ring || !lock()) return 0;
  int written = 0;
  size_t used = 0;
  // Walk oldest→newest.
  for (int i = 0; i < s_ringCount; i++) {
    int idx = (s_ringHead - s_ringCount + i + kRingLines * 2) % kRingLines;
    size_t len = strlen(s_ring[idx]);
    if (used + len + 2 >= cap) break;
    if (used) { out[used++] = '\n'; }
    memcpy(out + used, s_ring[idx], len);
    used += len;
    out[used] = '\0';
    written++;
  }
  unlock();
  return written;
}

void clearBreadcrumbCrash() {
  if (lock()) {
    s_crumb.hungTask[0] = '\0';
    unlock();
  }
}

}  // namespace telemetry
}  // namespace core
