#include "services/morning_quote.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>
#include <freertos/task.h>
#include <string.h>

#include "services/agent_transport.h"
#include "services/agent_coordination.h"
#include "core/device_agent_text.h"
#include "core/morning_quote_cache.h"
#include "core/store.h"
#include "hal/hal_sd.h"
#include "hal/hal_wifi.h"

namespace services {
namespace morning_quote {
namespace {

using Record = core::morning_quote_record::Record;
using Kind = core::morning_quote_record::Kind;

constexpr char kNvsNamespace[] = "morning_quote";
// Keep today's quote when prefetching tomorrow. The first key migrates the
// existing single-record cache without erasing or rewriting it at boot.
constexpr const char* kNvsKeys[] = {"current", "alternate"};
constexpr char kHistoryDir[] = "/morning";
constexpr char kHistoryPath[] = "/morning/quotes.jsonl";
constexpr uint32_t kAutomaticRetryMs = 15u * 60u * 1000u;
constexpr uint32_t kResourceRetryMs = 30u * 1000u;


portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
Record s_cached[2];
bool s_cachedValid[2] = {};
uint32_t s_targetDay = 0;
char s_targetDate[11] = {};
bool s_force = false;
Status s_status = Status::Idle;
char s_statusText[64] = "Idle";
bool s_taskRunning = false;
uint32_t s_requestedDay = 0;
char s_requestedDate[11] = {};
uint32_t s_requestGeneration = 0;
uint32_t s_requestRevision = 0, s_requestedRevision = 0;
bool s_newGeneration = false;
bool s_peripheralPaused = false;
uint32_t s_nextAttemptMs = 0;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool s_fixtureActive=false;
constexpr char kFixtureHistoryPath[]="/morning/agent-text-fixture-test.jsonl";
#endif

void setStatusLocked(Status value, const char* text) {
  s_status = value;
  strlcpy(s_statusText, text ? text : "", sizeof(s_statusText));
}

void setStatus(Status value, const char* text) {
  portENTER_CRITICAL(&s_mux);
  setStatusLocked(value, text);
  portEXIT_CRITICAL(&s_mux);
}

bool formatLocalDate(time_t epochSeconds, uint32_t* dayKey, char date[11]) {
  struct tm local{};
  if (epochSeconds <= 0 || !localtime_r(&epochSeconds, &local)) return false;
  const int year = local.tm_year + 1900;
  const int month = local.tm_mon + 1;
  const int day = local.tm_mday;
  if (year < 2000 || year > 2999 || month < 1 || month > 12 || day < 1 || day > 31) return false;
  if (dayKey) *dayKey = (uint32_t)(year * 10000 + month * 100 + day);
  if (date) snprintf(date, 11, "%04d-%02d-%02d", year, month, day);
  return true;
}

void publishRecord(const Record& record, int slot) {
  portENTER_CRITICAL(&s_mux);
  s_cached[slot] = record;
  s_cachedValid[slot] = true;
  portEXIT_CRITICAL(&s_mux);
}

bool storeRecord(const Record& record) {
  portENTER_CRITICAL(&s_mux);
  const int slot=core::morning_quote_record::replacementSlot(s_cached,s_cachedValid,record.dayKey);
  portEXIT_CRITICAL(&s_mux);
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  // A fixture reply is RAM-only. Reset or interrupted cleanup must never leave
  // simulated text in the persistent morning cache.
  if (s_fixtureActive) { publishRecord(record,slot); return true; }
#endif
  if(!core::store::putBlob(kNvsNamespace,kNvsKeys[slot],&record,sizeof(record))) return false;
  Record verify{};
  if(!core::store::getBlob(kNvsNamespace,kNvsKeys[slot],&verify,sizeof(verify)) ||
      memcmp(&verify,&record,sizeof(record))!=0) return false;
  publishRecord(record,slot);
  return true;
}

bool copyRecordForDay(uint32_t dayKey, Record* out) {
  Record copy{};
  bool held = false;
  portENTER_CRITICAL(&s_mux);
  for(int i=0;i<2;++i) {
    if (s_cachedValid[i] && core::morning_quote_record::matchesDay(s_cached[i],dayKey)) {
      copy = s_cached[i];
      held = true;
      break;
    }
  }
  portEXIT_CRITICAL(&s_mux);
  if (!held || !core::morning_quote_record::matchesDay(copy, dayKey)) return false;
  if (out) *out = copy;
  return true;
}

bool fillRecord(const char* text, uint32_t day, Record* out) {
  if (!text || !out) return false;
  const size_t length=strlen(text);
  // A short original reply is attributed to Kami. Never silently truncate or
  // accept partial stream text as the morning result.
  if (length<12 || length>180 || !core::morning_quote_record::printableAscii(text,length)) return false;
  *out=Record{};
  out->dayKey=day;
  out->kind=Kind::Philosophical;
  out->quoteLen=(uint8_t)length;
  memcpy(out->quote,text,length+1);
  strlcpy(out->attribution,"Kami",sizeof(out->attribution));
  out->attributionLen=4;
  return core::morning_quote_record::valid(*out);
}

bool appendHistory(const Record& record, const char* date) {
  if (!hal::sd::mounted() || !hal::sd::ensureDir(kHistoryDir)) return false;
  JsonDocument doc;
  doc["date"] = date;
  doc["quote"] = record.quote;
  doc["attribution"] = record.attribution;
  doc["kind"] = record.kind == Kind::Motivational ? "motivational" : "philosophical";
  if (record.idLen) doc["id"] = record.backendId;
  doc["cached_at"] = (uint32_t)time(nullptr);
  char line[512];
  const size_t len = serializeJson(doc, line, sizeof(line) - 2);
  if (len == 0 || len + 1 >= sizeof(line)) return false;
  line[len] = '\n';
  const char* path=kHistoryPath;
#ifdef ORIGAMI_DEV_SERIAL_INPUT
  if (s_fixtureActive) path=kFixtureHistoryPath;
#endif
  return hal::sd::appendFile(path, (const uint8_t*)line, len + 1);
}

}  // namespace

void begin() {
  s_requestGeneration=core::store::getU32(kNvsNamespace,"request_gen",0);
  setStatus(Status::Idle, "No cached quote");
  for(int slot=0;slot<2;++slot) {
    Record record{};
    if (core::store::getBlob(kNvsNamespace, kNvsKeys[slot], &record, sizeof(record)) &&
        core::morning_quote_record::valid(record)) {
      publishRecord(record,slot);
      setStatus(Status::Ready, "Cached quote loaded");
      Serial.printf("[morning-quote] loaded slot=%d day=%lu\n",slot,(unsigned long)record.dayKey);
    }
  }
}

uint32_t localDayKey(time_t epochSeconds) {
  uint32_t day = 0;
  return formatLocalDate(epochSeconds, &day, nullptr) ? day : 0;
}

void requestForEpoch(time_t epochSeconds, bool force) {
  uint32_t day = 0;
  char date[11] = {};
  if (!formatLocalDate(epochSeconds, &day, date)) return;
  const bool cached = !force && copyRecordForDay(day, nullptr);
  const bool online = hal::wifi::connected();
  portENTER_CRITICAL(&s_mux);
  // Re-entering the flow must not defeat the failed-request backoff or launch
  // another agent turn for the same upcoming day.
  if(!force && s_targetDay==day) {
    portEXIT_CRITICAL(&s_mux);
    return;
  }
  ++s_requestRevision;
  s_targetDay = day;
  memcpy(s_targetDate, date, sizeof(date));
  s_force = force;
  if (force) s_newGeneration=true;
  s_nextAttemptMs = 0;
  if (!s_taskRunning) {
    setStatusLocked(cached ? Status::Ready : (online ? Status::Idle : Status::WaitingForNetwork),
                    cached ? "Quote ready" : (online ? "Quote queued" : "Waiting for WiFi"));
  }
  portEXIT_CRITICAL(&s_mux);
}

void requestToday(bool force) {
  requestForEpoch(time(nullptr), force);
}

// Complete a failed admission without imposing its backoff on a newer request.
static void deferAttempt(uint32_t revision, uint32_t retryAt, const char* error) {
  portENTER_CRITICAL(&s_mux);
  s_taskRunning=false;
  const bool current=s_requestRevision==revision;
  s_nextAttemptMs=current ? retryAt : 0;
  setStatusLocked(current && error ? Status::Error : Status::Idle,
                  current && error ? error : "Quote queued");
  portEXIT_CRITICAL(&s_mux);
}

void serviceBackground(uint32_t nowMs) {
  portENTER_CRITICAL(&s_mux);
  const bool paused=s_peripheralPaused;
  const bool running=s_taskRunning;
  const uint32_t day=s_targetDay;
  const bool force=s_force;
  const uint32_t revision=s_requestRevision, retryAt=s_nextAttemptMs;
  char date[11];
  memcpy(date,s_targetDate,sizeof(date));
  portEXIT_CRITICAL(&s_mux);
  if (paused) {
    core::device_agent_text::cancel();
    portENTER_CRITICAL(&s_mux);
    s_taskRunning=false;
    portEXIT_CRITICAL(&s_mux);
    return;
  }
  if (running) {
    core::device_agent_text::Result response;
    if (!core::device_agent_text::take(response)) return;
    Record record;
    const bool valid=response.outcome==core::device_agent_text::Outcome::Ready &&
        fillRecord(response.text,s_requestedDay,&record);
    const bool stored=valid && storeRecord(record);
    if (stored) {
      const bool history=appendHistory(record,s_requestedDate);
      Serial.printf("[morning-quote] cached day=%lu source=agent-text history=%u\n",(unsigned long)s_requestedDay,history);
    }
    const char* error=response.outcome==core::device_agent_text::Outcome::Ready ?
        (valid ? "Quote cache write failed" : "Agent quote must be 12-180 ASCII chars") : response.error;
    portENTER_CRITICAL(&s_mux);
    s_taskRunning=false;
    if (s_requestRevision!=s_requestedRevision || s_newGeneration) {
      s_nextAttemptMs=0;
      setStatusLocked(Status::Idle,"Quote queued");
    } else if (stored) {
      s_force=false;
      setStatusLocked(Status::Ready,"Quote ready");
    } else {
      setStatusLocked(Status::Error,error);
      const bool transient=(valid && !stored) || response.retryable ||
          response.outcome==core::device_agent_text::Outcome::Unsupported;
      s_nextAttemptMs=nowMs+(transient ? kResourceRetryMs : kAutomaticRetryMs);
      // Resume transport failures with the same identity. A completed but
      // unsuitable reply needs a new turn on its next bounded retry.
      if (!transient) s_newGeneration=true;
    }
    portEXIT_CRITICAL(&s_mux);
    if (!stored) Serial.printf("[morning-quote] failed day=%lu: %s\n",(unsigned long)s_requestedDay,error);
    return;
  }
  if (!day || (!force && copyRecordForDay(day,nullptr)) ||
      (int32_t)(nowMs-retryAt)<0) return;
  if (!hal::wifi::connected()) {
    portENTER_CRITICAL(&s_mux);
    if (s_requestRevision==revision) setStatusLocked(Status::WaitingForNetwork,"Waiting for WiFi");
    portEXIT_CRITICAL(&s_mux);
    return;
  }
  // A background quote must not interrupt the file/OTA/network operation that
  // already owns a transport pause. Existing results are collected above even
  // while paused; deferred requests keep their identity and retry deadline.
  if (services::agent_coordination::isSessionActive() ||
      services::agent_coordination::suspendRequested()) return;
  // Mark ownership before IO so peripheral suspension waits for completion.
  portENTER_CRITICAL(&s_mux);
  if (s_peripheralPaused || s_requestRevision!=revision) { portEXIT_CRITICAL(&s_mux); return; }
  s_taskRunning=true;
  s_requestedRevision=revision;
  setStatusLocked(Status::Fetching,"Asking Kami");
  const bool advance=s_newGeneration;
  s_newGeneration=false;
  portEXIT_CRITICAL(&s_mux);
  if (advance) {
    const uint32_t next=s_requestGeneration+1;
    if (!core::store::putU32(kNvsNamespace,"request_gen",next)) {
      portENTER_CRITICAL(&s_mux);
      s_newGeneration=true;
      portEXIT_CRITICAL(&s_mux);
      deferAttempt(revision,nowMs+kResourceRetryMs,"Request identity save failed");
      return;
    }
    s_requestGeneration=next;
  }
  char requestId[81],prompt[600];
  snprintf(requestId,sizeof(requestId),"mq2:%lu:%lu",(unsigned long)day,(unsigned long)s_requestGeneration);
  snprintf(prompt,sizeof(prompt),
      "Automatic request for my morning routine on %s. Give me one brief, relevant piece of wisdom "
      "to start that day, using what you know about me when helpful. Write one original sentence of "
      "12 to 180 characters, printable ASCII only. Return only the sentence, no heading, quotation "
      "marks, attribution, markdown, or follow-up. Deliver it as one internal message to this device. "
      "The device needs a reply even if you answered today; resend it instead of staying silent. "
      "This is advance preparation, not something I just said or an event that already happened.",date);
  s_requestedDay=day; memcpy(s_requestedDate,date,sizeof(date));
  if (core::device_agent_text::submit(requestId,prompt)) {
    Serial.printf("[morning-quote] requested day=%lu source=agent-text id=%s\n",(unsigned long)day,requestId);
  } else {
    deferAttempt(revision,nowMs+kResourceRetryMs,nullptr);
  }
}

bool pollPeripheralPause() {
  portENTER_CRITICAL(&s_mux);
  s_peripheralPaused=true;
  const bool ready=!s_taskRunning;
  portEXIT_CRITICAL(&s_mux);
  return ready;
}

bool suspendForPeripheralIo(uint32_t timeoutMs) {
  const uint32_t started=millis();
  while (!pollPeripheralPause()) {
    if(millis()-started>=timeoutMs) {
      resumeAfterPeripheralIo();
      return false;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return true;
}

void resumeAfterPeripheralIo() {
  portENTER_CRITICAL(&s_mux);
  s_peripheralPaused=false;
  portEXIT_CRITICAL(&s_mux);
}

bool getForDay(uint32_t dayKey, Record* out) {
  return copyRecordForDay(dayKey, out);
}

bool getForToday(Record* out) {
  return getForDay(localDayKey(time(nullptr)), out);
}

Status status() {
  portENTER_CRITICAL(&s_mux);
  const Status value=s_status;
  portEXIT_CRITICAL(&s_mux);
  return value;
}

const char* statusText() {
  static char copy[64];
  portENTER_CRITICAL(&s_mux);
  strlcpy(copy, s_statusText, sizeof(copy));
  portEXIT_CRITICAL(&s_mux);
  return copy;
}

bool fetching() { return status() == Status::Fetching; }

#ifdef ORIGAMI_DEV_SERIAL_INPUT
bool fixtureCacheForDevelopment(bool restore) {
  if (!suspendForPeripheralIo(3000)) return false;
  s_fixtureActive=!restore;
  if (restore) {
    hal::sd::removeFile(kFixtureHistoryPath);
    missCacheForDevelopment();
    begin();
  }
  resumeAfterPeripheralIo();
  return true;
}

bool restoreCacheFileForDevelopment() {
  constexpr const char* path="/morning/quote-cache-recovery.bin";
  if (!suspendForPeripheralIo(3000)) return false;
  Record records[2];
  hal::sd::StreamReader reader{};
  bool ok=hal::sd::openStream(path,&reader);
  if (ok) {
    ok=reader.size==sizeof(records) &&
       hal::sd::readStream(&reader,reinterpret_cast<uint8_t*>(records),sizeof(records))==sizeof(records);
    hal::sd::closeStream(&reader);
  }
  if (ok) for (const auto& record:records)
    ok=ok && core::morning_quote_record::valid(record) && strncmp(record.quote,"[TEST]",6)!=0;
  if (ok) for (int i=0;i<2;++i) {
    Record verify{};
    ok=core::store::putBlob(kNvsNamespace,kNvsKeys[i],&records[i],sizeof(Record)) &&
       core::store::getBlob(kNvsNamespace,kNvsKeys[i],&verify,sizeof(Record)) &&
       memcmp(&verify,&records[i],sizeof(Record))==0 && ok;
  }
  if (ok) {
    s_fixtureActive=false;
    hal::sd::removeFile(path);
    hal::sd::removeFile(kFixtureHistoryPath);
    missCacheForDevelopment();begin();
    Serial.println("[morning-fixture] cache recovery verified");
  }
  resumeAfterPeripheralIo();
  return ok;
}

bool missCacheForDevelopment() {
  portENTER_CRITICAL(&s_mux);
  if(s_taskRunning) { portEXIT_CRITICAL(&s_mux); return false; }
  s_cachedValid[0]=s_cachedValid[1]=false;
  ++s_requestRevision;
  s_targetDay=0; s_force=false; s_nextAttemptMs=0;
  s_status=Status::Idle;
  strlcpy(s_statusText,"No cached quote",sizeof(s_statusText));
  portEXIT_CRITICAL(&s_mux);
  Serial.println("[morning-regression] RAM cache miss; persisted quotes unchanged");
  return true;
}

bool seedTodayForDevelopment() {
  uint32_t day = 0;
  char date[11] = {};
  if (!formatLocalDate(time(nullptr), &day, date)) {
    Serial.println("[morning-quote] development seed failed: local date unavailable");
    return false;
  }
  Record record{};
  record.dayKey = day;
  record.kind = Kind::Motivational;
  strlcpy(record.quote,
          "Begin gently; a clear direction matters more than a hurried start.",
          sizeof(record.quote));
  strlcpy(record.attribution, "Kami", sizeof(record.attribution));
  record.quoteLen = (uint8_t)strlen(record.quote);
  record.attributionLen = (uint8_t)strlen(record.attribution);
  if (!core::morning_quote_record::valid(record)) {
    Serial.println("[morning-quote] development seed failed: invalid fixture record");
    return false;
  }
  if (!storeRecord(record)) {
    Serial.printf("[morning-quote] development seed failed: NVS write free_entries=%u\n",
                  (unsigned)core::store::freeEntries(kNvsNamespace));
    return false;
  }
  const bool history = appendHistory(record, date);
  setStatus(Status::Ready, history ? "Development quote ready" : "Quote ready; SD history unavailable");
  Serial.printf("[morning-quote] development seed day=%lu history=%u free_entries=%u\n",
                (unsigned long)day, history,
                (unsigned)core::store::freeEntries(kNvsNamespace));
  return true;
}
#endif

}  // namespace morning_quote
}  // namespace services
