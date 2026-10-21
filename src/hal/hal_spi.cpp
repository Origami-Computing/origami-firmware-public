#include "hal/hal_spi.h"
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

namespace hal {
namespace spi {

static SemaphoreHandle_t s_mutex = nullptr;

static void ensureMutex() {
  if (!s_mutex) {
    s_mutex = xSemaphoreCreateMutex();
  }
}

void lock() {
  ensureMutex();
  if (s_mutex) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
}

void unlock() {
  if (s_mutex) {
    xSemaphoreGive(s_mutex);
  }
}

}  // namespace spi
}  // namespace hal

