#include "core/telemetry_queue.h"
#include "core/usage_events.h"

#include <Arduino.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>
#include <atomic>

#include "hal/hal_bus.h"
#include "hal/hal_sd.h"

namespace core {
namespace telemetry_queue {

namespace {

constexpr char kDir[]     = "/telemetry_queue";
constexpr char kJournal[] = "/telemetry_queue/journal.jsonl";
constexpr char kTmp[]     = "/telemetry_queue/journal.tmp";

// Ring of pending events (count <= kMaxItems). Allocated in PSRAM, NOT internal
// DRAM: the queue now uses ~161 KiB. Even the original ~8 KiB internal
// allocation starved esp_wifi_init of the contiguous internal heap it needs to re-init the radio
// after a WiFi drop (observed NO_MEM → device stuck offline). PSRAM has MBs free
// and this buffer is only touched by tasks (no ISR/DMA), so PSRAM is safe here.
Item*             s_items = nullptr;
int               s_count = 0;
SemaphoreHandle_t s_mutex = nullptr;
std::atomic<uint32_t> s_droppedVerbose{0}, s_droppedDurable{0}, s_journalFailures{0};

bool lock(uint32_t timeoutMs = 200) {
  // s_items null = PSRAM alloc failed at init → treat queue as unavailable.
  return s_items && s_mutex && xSemaphoreTake(s_mutex, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}
void unlock() {
  if (s_mutex) xSemaphoreGive(s_mutex);
}

// Rewrite the SD journal to exactly match the in-RAM ring. Caller holds s_mutex.
// Uses temp+rename so a power loss mid-write can't truncate the live journal.
constexpr char kBackup[] = "/telemetry_queue/journal.bak";
void rewriteJournalUnlocked() {
  if (!hal::sd::mounted() || !hal::bus::lockSpi(pdMS_TO_TICKS(500))) { ++s_journalFailures; return; }
  SD.remove(kTmp);
  File file = SD.open(kTmp, FILE_WRITE);
  bool ok = !!file;
  if (file) {
    for (int i=0; i<s_count && ok; ++i) {
      if (s_items[i].kind==KIND_VERBOSE || s_items[i].kind==KIND_HEARTBEAT) continue;
      const size_t expected=2+strlen(s_items[i].json)+1;
      ok=file.printf("%u\t%s\n",(unsigned)s_items[i].kind,s_items[i].json)==expected;
    }
    file.flush(); file.close();
  }
  if (ok) {
    // If a prior promotion failed, recover first. Never append to a new empty
    // journal while the only durable generation is still in the backup.
    if (!SD.exists(kJournal) && SD.exists(kBackup)) ok=SD.rename(kBackup,kJournal);
    if (ok && SD.exists(kJournal)) {
      if (SD.exists(kBackup)) ok=SD.remove(kBackup);
      if (ok) ok=SD.rename(kJournal,kBackup);
    }
    if (ok) ok=SD.rename(kTmp,kJournal);
    if (ok) SD.remove(kBackup);
  }
  if (!ok) ++s_journalFailures;
  hal::bus::unlockSpi();
}

void appendJournalUnlocked(const Item& it) {
  if (!hal::sd::mounted() || !hal::bus::lockSpi(pdMS_TO_TICKS(200))) { ++s_journalFailures; return; }
  bool ok=true;
  if (!SD.exists(kJournal) && SD.exists(kBackup)) ok=SD.rename(kBackup,kJournal);
  File file;
  if(ok) file=SD.open(kJournal,FILE_APPEND);
  if (file) {
    ok=file.printf("%u\t%s\n",(unsigned)it.kind,it.json)==2+strlen(it.json)+1;
    file.flush(); file.close();
  } else ok=false;
  if (!ok) ++s_journalFailures;
  hal::bus::unlockSpi();
}

// Push into the RAM ring, dropping the oldest if full. Caller holds s_mutex.
void pushUnlocked(uint8_t kind, const char* json) {
  if (s_count >= kMaxItems) {
    // Drop oldest (shift down). kMaxItems is small so the memmove is cheap.
    memmove(&s_items[0], &s_items[1], sizeof(Item) * (kMaxItems - 1));
    s_count = kMaxItems - 1;
  }
  Item& slot = s_items[s_count++];
  slot.kind = kind;
  strlcpy(slot.json, json ? json : "", sizeof(slot.json));
}

}  // namespace

void init() {
  core::usage::init();
  if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
  // Allocate the ring in PSRAM (keeps internal DRAM free for WiFi re-init). If
  // PSRAM is somehow unavailable, the queue stays disabled rather than eating
  // internal heap.
  if (!s_items) {
    s_items = (Item*)heap_caps_malloc(sizeof(Item) * kMaxItems, MALLOC_CAP_SPIRAM);
    if (!s_items) {
      Serial.println("[telemetry_q] PSRAM alloc failed — queue disabled");
      return;
    }
  }
  if (!lock()) return;
  s_count = 0;

  if (!hal::sd::mounted()) {
    unlock();
    Serial.println("[telemetry_q] SD unavailable; using volatile queue");
    return;
  }

  if (hal::bus::lockSpi(pdMS_TO_TICKS(500))) {
    if (!SD.exists(kDir)) SD.mkdir(kDir);
    // Replay any events that were journaled but never sent.
    if (!SD.exists(kJournal) && SD.exists(kBackup)) SD.rename(kBackup,kJournal);
    File f = SD.open(kJournal, FILE_READ);
    if (f) {
      while (f.available() && s_count < kMaxItems) {
        String line = f.readStringUntil('\n');
        if (line.length() < 3) continue;
        int tab = line.indexOf('\t');
        if (tab <= 0) continue;
        uint8_t kind = (uint8_t)line.substring(0, tab).toInt();
        // A prior boot's heartbeat would make stale firmware/health look current.
        // Only logs (with their device timestamp) and crash reports are durable.
        if (kind == KIND_HEARTBEAT || kind == KIND_VERBOSE) continue;
        String  json = line.substring(tab + 1);
        Item& slot = s_items[s_count++];
        slot.kind = kind;
        strlcpy(slot.json, json.c_str(), sizeof(slot.json));
      }
      f.close();
    }
    hal::bus::unlockSpi();
  }
  const int loaded = s_count;
  // Compact the on-disk journal to match what we actually loaded.
  rewriteJournalUnlocked();
  unlock();
  Serial.printf("[telemetry_q] init: replayed %d pending event(s)\n", loaded);
}

void enqueue(Kind kind, const char* json) {
  auto& dropped = kind == KIND_VERBOSE ? s_droppedVerbose : s_droppedDurable;
  if (!json || strlen(json)>=kItemJsonCap) { ++dropped; return; }
  if (!lock(kind == KIND_VERBOSE ? 0 : 200)) { ++dropped; return; }
  // Never evict the in-flight snapshot: ack(n) must still refer to its prefix.
  // Verbose traffic cannot evict crashes/heartbeats or churn the SD journal.
  if (s_count >= (kind == KIND_VERBOSE ? kMaxItems - 8 : kMaxItems)) { ++dropped; unlock(); return; }
  pushUnlocked((uint8_t)kind, json);
  if (kind != KIND_VERBOSE && kind != KIND_HEARTBEAT) appendJournalUnlocked(s_items[s_count - 1]);
  unlock();
}

bool hasPending() {
  if (!lock()) return false;
  const bool any = s_count > 0;
  unlock();
  return any;
}

int snapshot(Item* out, int maxN) {
  if (!out || maxN <= 0) return 0;
  if (!lock()) return 0;
  int n = s_count < maxN ? s_count : maxN;
  for (int i = 0; i < n; i++) out[i] = s_items[i];
  unlock();
  return n;
}

void ack(int n) {
  if (n <= 0) return;
  if (!lock()) return;
  if (n > s_count) n = s_count;
  memmove(&s_items[0], &s_items[n], sizeof(Item) * (s_count - n));
  s_count -= n;
  rewriteJournalUnlocked();
  unlock();
}

Stats stats() {
  uint32_t queued = 0;
  if (lock()) { queued = s_count; unlock(); }
  return {queued, s_droppedVerbose.load(), s_droppedDurable.load(), s_journalFailures.load()};
}

}  // namespace telemetry_queue
}  // namespace core
