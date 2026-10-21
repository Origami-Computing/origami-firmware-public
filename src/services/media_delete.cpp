#include "services/media_delete.h"

#include <Arduino.h>

#include "services/uploads.h"
#include "hal/hal_sd.h"

namespace services {
namespace media_delete {

bool deleteFile(const char* path, Result* result) {
  if (result) result->requested++;
  if (!path || !path[0]) {
    if (result) result->failed++;
    return false;
  }

  const bool queueRemoved = services::uploads::queue().removeByFilePath(path);
  if (queueRemoved && result) result->queueRemoved++;

  if (!hal::sd::exists(path)) {
    if (result) result->missing++;
    Serial.printf("[media-delete] Missing, queue-cleaned=%d: %s\n",
                  queueRemoved ? 1 : 0, path);
    return true;
  }

  if (!hal::sd::removeFile(path)) {
    if (result) result->failed++;
    Serial.printf("[media-delete] Delete failed: %s\n", path);
    return false;
  }

  if (result) result->removed++;
  Serial.printf("[media-delete] Deleted %s queue-cleaned=%d\n",
                path, queueRemoved ? 1 : 0);
  return true;
}

}  // namespace media_delete
}  // namespace services
