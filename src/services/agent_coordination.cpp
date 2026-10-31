#include "services/agent_coordination.h"
#include <Arduino.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace services { namespace agent_coordination {
namespace {
std::atomic<bool> started{false};
std::atomic<TaskHandle_t> worker{nullptr};
// Count and acknowledgment change atomically: a new first owner cannot inherit
// an acknowledgment from a completed pause while the worker resumes socket work.
constexpr uint32_t kAcknowledged = uint32_t(1) << 31;
constexpr uint32_t kOwners = kAcknowledged - 1;
std::atomic<uint32_t> pauseState{0};
std::atomic<bool> active{false};
std::atomic<WorkerPhase> phase{WorkerPhase::NotStarted};
}
void setWorkerPhase(WorkerPhase value) { phase.store(value); }
void workerStarted() { started.store(true); }
void bindCurrentWorker() {
    worker.store(xTaskGetCurrentTaskHandle());
    started.store(true); // Publish before this task performs its first socket call.
}
bool isCurrentWorker() {
    const TaskHandle_t owner = worker.load();
    return owner && owner == xTaskGetCurrentTaskHandle();
}
bool cancelHandshakeForCurrentTask() {
    return isCurrentWorker() && suspendRequested();
}
bool suspendRequested() { return (pauseState.load() & kOwners) != 0; }
void acknowledgeSuspended() {
    // Worker calls this only after socket cleanup and returns to its paused loop.
    // Do not leave an acknowledgment behind when the last owner has gone away.
    uint32_t state = pauseState.load();
    while ((state & kOwners) &&
           !pauseState.compare_exchange_weak(state, state | kAcknowledged)) {}
}
void setSessionActive(bool value) { active.store(value); }
bool isSessionActive() { return active.load(); }

bool TransportPause::pollAcquire() {
    if (!held_) {
        uint32_t state = pauseState.load();
        do {
            if ((state & kOwners) == kOwners) return false;
        } while (!pauseState.compare_exchange_weak(state, state + 1));
        held_ = true;
    }
    return !started.load() || (pauseState.load() & kAcknowledged) != 0;
}

bool TransportPause::acquire(uint32_t timeoutMs) {
    if (pollAcquire()) return true;
    if (!held_) return false;
    const uint32_t startMs = millis();
    while (!(pauseState.load() & kAcknowledged) &&
           (uint32_t)(millis() - startMs) < timeoutMs) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const bool ok = (pauseState.load() & kAcknowledged) != 0;
    if (!ok) {
        Serial.printf("[agent-pause] timeout phase=%u requested=%u active=%u\n",
                      (unsigned)phase.load(), suspendRequested()?1u:0u, active.load()?1u:0u);
        release();
        Serial.printf("[agent] WiFi suspend pause timed out after %lu ms\n",
                      (unsigned long)(millis() - startMs));
    }
    return ok;
}
void TransportPause::release() {
    if (!held_) return;
    held_ = false;
    uint32_t state = pauseState.load();
    while (!pauseState.compare_exchange_weak(state,
            (state & kOwners) == 1 ? 0 : state - 1)) {}
}
} }
