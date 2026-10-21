#include "net_poll_lifecycle.h"
#include "services/net_poll.h"
#include "hal/hal_tasks.h"
#include "core/telemetry.h"
#include <Arduino.h>
#include <atomic>
#include <freertos/portmacro.h>

namespace services { namespace net_poll {
namespace {
enum class Phase { Idle, Starting, Running, Retiring };
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
Phase phase = Phase::Idle;
TaskHandle_t handle = nullptr;
std::atomic<bool> stopIntent{false};
uint32_t notifications = 0;
bool restartPending = false;

bool tryStart(bool requestedOnly) {
  portENTER_CRITICAL(&mux);
  if (requestedOnly && !restartPending) {
    portEXIT_CRITICAL(&mux);
    return false;
  }
  if (phase == Phase::Running) {
    restartPending = false;
    stopIntent.store(false);
    portEXIT_CRITICAL(&mux);
    return true;
  }
  if (phase != Phase::Idle) {
    portEXIT_CRITICAL(&mux);
    return false; // Creation/irreversible retirement is still in progress. Retry.
  }
  const bool restoreOnFailure = restartPending;
  restartPending = false; // Consume intent atomically with task admission.
  phase = Phase::Starting;
  stopIntent.store(false);
  portEXIT_CRITICAL(&mux);

#ifdef AUDIO_UPLOAD_DISABLE_NET_POLL_DIAGNOSTIC
  static bool logged = false;
  if (!logged) {
    Serial.println("[net_poll] disabled for audio-upload diagnostic build");
    logged = true;
  }
  portENTER_CRITICAL(&mux);
  phase = Phase::Idle;
  portEXIT_CRITICAL(&mux);
  return true;
#endif

  TaskHandle_t created = hal::tasks::spawn(hal::tasks::Id::NetPoll, lifecycle::run);
  portENTER_CRITICAL(&mux);
  handle = created;
  phase = created ? Phase::Running : Phase::Idle;
  // Retry allocation failure unless a later stop cancelled this admission.
  if (!created && restoreOnFailure && !stopIntent.load()) restartPending = true;
  portEXIT_CRITICAL(&mux);
  if (!created) {
    Serial.println("[net_poll] FAILED to start");
    return false;
  }
  LOGI("net_poll", "Started on Core 0");
  return true;
}

} // namespace

bool start() { return tryStart(false); }
bool startRequested() { return tryStart(true); }
void requestStart() {
  portENTER_CRITICAL(&mux);
  restartPending = true;
  portEXIT_CRITICAL(&mux);
}

void requestStop() {
  portENTER_CRITICAL(&mux);
  restartPending = false;
  stopIntent.store(true);
  if (phase == Phase::Idle) {
    portEXIT_CRITICAL(&mux);
    return;
  }
  TaskHandle_t notified = phase == Phase::Running ? handle : nullptr;
  if (notified) ++notifications;
  portEXIT_CRITICAL(&mux);
  if (notified) {
    xTaskNotifyGive(notified); // Borrow remains valid through this RTOS call.
    portENTER_CRITICAL(&mux);
    --notifications;
    portEXIT_CRITICAL(&mux);
  }
}

bool stop(uint32_t timeoutMs) {
  requestStop();
  const uint32_t startedMs = millis();
  while (running() && uint32_t(millis() - startedMs) < timeoutMs)
    vTaskDelay(pdMS_TO_TICKS(10));
  if (running()) {
    Serial.printf("[net_poll] stop timed out after %lu ms\n", (unsigned long)(millis() - startedMs));
    return false;
  }
  Serial.printf("[net_poll] stopped in %lu ms\n", (unsigned long)(millis() - startedMs));
  return true;
}

bool running() {
  portENTER_CRITICAL(&mux);
  const bool active = phase != Phase::Idle;
  portEXIT_CRITICAL(&mux);
  return active;
}
bool stopRequested() { return stopIntent.load(); }

namespace lifecycle {
void awaitPublication() {
  for (;;) {
    portENTER_CRITICAL(&mux);
    const bool pending = phase == Phase::Starting;
    portEXIT_CRITICAL(&mux);
    if (!pending) return;
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
bool claimRetirement() {
  portENTER_CRITICAL(&mux);
  const bool retire = phase == Phase::Running && stopIntent.load();
  if (retire) phase = Phase::Retiring;
  portEXIT_CRITICAL(&mux);
  return retire;
}
void publishQuiescence() {
  for (;;) {
    portENTER_CRITICAL(&mux);
    if (!notifications) {
      handle = nullptr;
      phase = Phase::Idle;
      portEXIT_CRITICAL(&mux);
      return; // No owner state access after this; only vTaskDelete(nullptr).
    }
    portEXIT_CRITICAL(&mux);
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}
} // namespace lifecycle
} }
