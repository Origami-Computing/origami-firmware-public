#include "services/uploads.h"
#include "services/background_network.h"
#include "hal/hal_sd.h"
#include "core/watchdog.h"
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace services { namespace uploads {
namespace {
UploadQueue s_queue;
std::atomic<bool> s_inited{false};
std::atomic_flag s_initializing = ATOMIC_FLAG_INIT;
bool s_reportedNoSd = false;
}

bool ensureInit() {
  if (s_inited.load()) return true;
  // UI and svc_task may arrive together. Serialize the entire setup attempt,
  // including callback installation, and publish ready only after queue init.
  while (s_initializing.test_and_set()) {
    core::watchdog::feed();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  bool initialized = s_inited.load();
  if (!initialized) {
    if (!hal::sd::mounted()) {
      if (!s_reportedNoSd) {
        Serial.println("[queue] SD capability unavailable; background uploads disabled");
        s_reportedNoSd = true;
      }
    } else {
      s_reportedNoSd = false;
      // Read NVS-backed credentials per transfer, not into duplicate DRAM strings.
      s_queue.setNetworkCoordinator(background_network::acquireUploadNetwork, background_network::releaseUploadNetwork);
      initialized = s_queue.init();
      if (initialized) s_inited.store(true);
    }
  }
  s_initializing.clear();
  return initialized;
}

UploadQueue& queue() {
  ensureInit();
  return s_queue;
}

UploadQueue* readyQueue() { return s_inited.load() ? &s_queue : nullptr; }

void tick() {
  if (s_inited && !background_network::audioPriorityActive()) s_queue.tick();
}

} }
