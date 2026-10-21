#include "hal/hal_bus.h"
#include <atomic>
#include <cstdlib>

namespace hal {
namespace bus {
namespace {
StaticSemaphore_t spiStorage, i2cStorage;
std::atomic<SemaphoreHandle_t> s_spiMutex{nullptr}, s_i2cMutex{nullptr};
portMUX_TYPE initializationMux = portMUX_INITIALIZER_UNLOCKED;

SemaphoreHandle_t ensureMutex(std::atomic<SemaphoreHandle_t>& slot,
                              StaticSemaphore_t& storage, bool recursive) {
  SemaphoreHandle_t mutex = slot.load();
  if (mutex) return mutex;
  // Static creation only initializes kernel bookkeeping; no heap allocation or
  // blocking wait. Publish once, including concurrent first use on both cores.
  portENTER_CRITICAL(&initializationMux);
  mutex = slot.load();
  if (!mutex) {
    mutex = recursive ? xSemaphoreCreateRecursiveMutexStatic(&storage)
                      : xSemaphoreCreateMutexStatic(&storage);
    slot.store(mutex);
  }
  portEXIT_CRITICAL(&initializationMux);
  // With valid static storage the pinned RTOS promises a handle. Never let an
  // unexpected kernel/setup failure authorize unprotected I/O, even for callers
  // using an indefinite wait without checking its result.
  if (!mutex) abort();
  return mutex;
}
} // namespace

SemaphoreHandle_t spiMutex() {
  // Direct-draw helpers can nest within a same-task TFT/SD bus scope.
  return ensureMutex(s_spiMutex, spiStorage, true);
}

SemaphoreHandle_t i2cMutex() {
  return ensureMutex(s_i2cMutex, i2cStorage, false);
}

bool lockSpi(TickType_t timeout) {
  return xSemaphoreTakeRecursive(spiMutex(), timeout) == pdTRUE;
}

void unlockSpi() {
  if (auto mutex = s_spiMutex.load()) xSemaphoreGiveRecursive(mutex);
}

bool lockI2c(TickType_t timeout) {
  return xSemaphoreTake(i2cMutex(), timeout) == pdTRUE;
}

void unlockI2c() {
  if (auto mutex = s_i2cMutex.load()) xSemaphoreGive(mutex);
}

} // namespace bus
} // namespace hal
