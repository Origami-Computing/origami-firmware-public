#include "core/watchdog.h"

#include <Arduino.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

#include "core/menu_config.h"  // TWDT_TIMEOUT_MS

namespace core {
namespace watchdog {

namespace {

constexpr uint32_t kMagic    = 0x57445347;  // 'WDSG'
constexpr int      kMaxTasks = 6;  // ui, svc, net_poll, uploadq (+headroom)

struct Entry {
  TaskHandle_t      handle;       // valid only within the run that set it
  char              name[16];
  volatile uint32_t lastFeedMs;
  bool              active;       // false once unsubscribed (dynamic tasks) — excluded from blame
};

struct State {
  uint32_t magic;
  int      count;
  Entry    e[kMaxTasks];
};

// RTC_NOINIT: survives panic/WDT/soft reset (lost only on full power-off), so a
// crash can attribute the hung task on the next boot.
RTC_NOINIT_ATTR State s_wd;

int indexOfCurrentTask() {
  TaskHandle_t h = xTaskGetCurrentTaskHandle();
  for (int i = 0; i < s_wd.count; i++) {
    // Retained worker stacks also reuse their StaticTask_t address. A retired
    // task's handle must not capture feeds from its successor under a new name.
    if (s_wd.e[i].active && s_wd.e[i].handle == h) return i;
  }
  return -1;
}

}  // namespace

void init() {
  // Convert ms→s (legacy API granularity); never below 1 s.
  uint32_t sec = (TWDT_TIMEOUT_MS + 999u) / 1000u;
  if (sec < 1) sec = 1;
  // Arduino may have auto-initialized the TWDT for the idle/loop tasks. Take
  // ownership so our timeout/panic config applies; ignore errors if we can't.
  esp_task_wdt_deinit();
  esp_err_t err = esp_task_wdt_init(sec, /*panic=*/true);
  Serial.printf("[wdt] init timeout=%us panic=1 (err=%d)\n", (unsigned)sec, (int)err);

  // Core 0 runs every TLS consumer. Verified handshakes are CPU-bound (about
  // 6 s each on a P-384 chain), so two overlapping workers can legitimately
  // keep core 0 busy past the timeout without anything hanging. Starvation of
  // the idle task is therefore not a fault signal here (Arduino's
  // disableCore0WDT). Every worker still subscribes itself and keeps its own
  // deadline, so a real hang in net_poll, svc, uploads or ui still resets.
  const esp_err_t idle = esp_task_wdt_delete(xTaskGetIdleTaskHandleForCPU(0));
  Serial.printf("[wdt] core0 idle check off (err=%d)\n", (int)idle);

  // Reset the attribution table for THIS run (previous run already read).
  s_wd.magic = kMagic;
  s_wd.count = 0;
}

void subscribe() {
  if (esp_task_wdt_add(NULL) != ESP_OK) {
    Serial.println("[wdt] subscribe failed");
  }
  TaskHandle_t h  = xTaskGetCurrentTaskHandle();
  const char*  nm = pcTaskGetName(NULL);
  if (!nm) nm = "?";
  // Reuse a slot with the same name so tasks that are destroyed and recreated
  // (e.g. uploadq across voice interactions) don't grow the table unbounded.
  for (int i = 0; i < s_wd.count; i++) {
    if (strcmp(s_wd.e[i].name, nm) == 0) {
      s_wd.e[i].handle     = h;
      s_wd.e[i].lastFeedMs = millis();
      s_wd.e[i].active     = true;
      Serial.printf("[wdt] re-subscribed '%s'\n", s_wd.e[i].name);
      return;
    }
  }
  if (s_wd.count >= kMaxTasks) return;
  Entry& e = s_wd.e[s_wd.count++];
  e.handle = h;
  strlcpy(e.name, nm, sizeof(e.name));
  e.lastFeedMs = millis();
  e.active = true;
  Serial.printf("[wdt] subscribed '%s'\n", e.name);
}

void unsubscribe() {
  // Dynamic tasks (uploadq) MUST call this before vTaskDelete, else the TWDT
  // keeps expecting the now-dead task to feed and would falsely reset.
  esp_task_wdt_delete(NULL);
  TaskHandle_t h = xTaskGetCurrentTaskHandle();
  for (int i = 0; i < s_wd.count; i++) {
    if (s_wd.e[i].active && s_wd.e[i].handle == h) {
      s_wd.e[i].active = false;
      s_wd.e[i].handle = nullptr;
      break;
    }
  }
}

void feed() {
  esp_task_wdt_reset();
  int i = indexOfCurrentTask();
  if (i >= 0) s_wd.e[i].lastFeedMs = millis();
}

const char* hungTaskGuess() {
  if (s_wd.magic != kMagic || s_wd.count <= 0) return "";
  int      stalest = -1;
  uint32_t oldest  = 0;
  for (int i = 0; i < s_wd.count; i++) {
    if (!s_wd.e[i].active) continue;  // a task that unsubscribed wasn't being watched
    if (stalest < 0 || s_wd.e[i].lastFeedMs < oldest) {
      oldest  = s_wd.e[i].lastFeedMs;
      stalest = i;
    }
  }
  return stalest >= 0 ? s_wd.e[stalest].name : "";
}

}  // namespace watchdog
}  // namespace core
