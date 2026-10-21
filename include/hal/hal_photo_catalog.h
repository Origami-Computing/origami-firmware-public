#pragma once

#include <stddef.h>
#include <stdint.h>

namespace hal {
namespace sd {

namespace photo_policy {
constexpr char lower(char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; }
constexpr bool equal(const char* a, const char* b) {
  return lower(*a) == lower(*b) && (*a == 0 || equal(a + 1, b + 1));
}
constexpr bool extension(const char* name) {
  return *name && ((*name == '.' && (equal(name, ".jpg") || equal(name, ".jpeg"))) ||
                   extension(name + 1));
}
constexpr bool isPhoto(const char* name) {
  return name && *name && *name != '.' && extension(name);
}
constexpr int compareName(const char* a, const char* b) {
  return lower(*a) == lower(*b) ? (*a ? compareName(a + 1, b + 1) : 0) :
         (lower(*a) < lower(*b) ? -1 : 1);
}
constexpr const char* basename(const char* path, const char* start) {
  return *path ? basename(path + 1, *path == '/' ? path + 1 : start) : start;
}
constexpr uint64_t sequenceDigits(const char* digits, uint64_t value = 0) {
  return *digits >= '0' && *digits <= '9' ?
      (value > (UINT64_MAX - (*digits - '0')) / 10 ? 0 :
       sequenceDigits(digits + 1, value * 10 + (*digits - '0'))) :
      (equal(digits, ".jpg") || equal(digits, ".jpeg") ? value : 0);
}
constexpr uint64_t sequenceName(const char* name) {
  return lower(name[0]) == 'i' && lower(name[1]) == 'm' && lower(name[2]) == 'g' && name[3] == '_' ?
      sequenceDigits(name + 4) : 0;
}
constexpr uint64_t sequence(const char* path) { return sequenceName(basename(path, path)); }
constexpr int newestFirst(uint64_t aTime, const char* aName, uint64_t bTime, const char* bName) {
  // Camera IDs remain monotonic even before the clock synchronizes. Put those
  // captures first; use time/name order within the remaining imported/test files.
  return sequence(aName) != sequence(bName) ? (sequence(aName) > sequence(bName) ? -1 : 1) :
         aTime != bTime ? (aTime > bTime ? -1 : 1) : -compareName(aName, bName);
}
}  // namespace photo_policy

struct PhotoFile {
  char path[272];  // FAT long filename plus /photos/ and terminator.
  uint64_t modified;
};

enum class PhotoCatalogResult { Ok, NoSd, ReadError, NoMemory };

struct PhotoCatalogTimings {
  uint32_t totalUs = 0;
  uint32_t allocationUs = 0;
  uint32_t metadataReads = 0;
};

class PhotoCatalog {
 public:
  PhotoCatalog() = default;
  ~PhotoCatalog() { clear(); }
  PhotoCatalog(const PhotoCatalog&) = delete;
  PhotoCatalog& operator=(const PhotoCatalog&) = delete;
  PhotoCatalogResult load();
  // Caller must keep background SD writers paused, as for load().
  bool removeAt(size_t index);
  void clear();
  size_t count() const { return count_; }
  const PhotoCatalogTimings& timings() const { return timings_; }
  const PhotoFile& operator[](size_t index) const { return files_[index]; }

 private:
  PhotoFile* files_ = nullptr;
  size_t count_ = 0;
  PhotoCatalogTimings timings_;
};

}  // namespace sd
}  // namespace hal
