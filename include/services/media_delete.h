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
 * Refuse active uploads; remove pending queue references and confirm the queue
 * manifest before removing the SD file. Hold an enqueue exclusion through file
 * removal. Failed queue persistence does not permit deletion; queue cleanup in
 * RAM is not rolled back. The queue service must initialize successfully.
 * Returns true when the file was removed or was already missing.
 */
bool deleteFile(const char* path, Result* result = nullptr);

}  // namespace media_delete
}  // namespace services
