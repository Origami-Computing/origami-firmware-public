#pragma once

/**
 * @file media_delete.h
 * @brief Recording/reflection deletion with upload-queue cleanup.
 *
 * Keep app delete flows going through this helper instead of calling SD.remove()
 * directly. Queue cleanup precedes file removal and is not rolled back on failure.
 * Photos retain their separate catalog-first deletion policy.
 */

namespace services {
namespace media_delete {

struct Result {
  int requested = 0;
  int removed = 0;
  int missing = 0;
  int queueRemoved = 0;
  int failed = 0;
};

/**
 * Remove one matching queue item, then remove the SD file if present.
 * Returns true when the file was removed or was already missing.
 */
bool deleteFile(const char* path, Result* result = nullptr);

}  // namespace media_delete
}  // namespace services
