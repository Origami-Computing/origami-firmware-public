#include "hal/hal_photo_catalog.h"

#include <SD.h>
#include <stdlib.h>
#include <string.h>

#include "core/memory.h"
#include "hal/hal_bus.h"
#include "hal/hal_sd.h"

namespace hal {
namespace sd {

namespace {
struct SpiLock {
  SpiLock() { hal::bus::lockSpi(); }
  ~SpiLock() { hal::bus::unlockSpi(); }
};

const char* basename(const char* path) {
  if (!path) return "";
  const char* slash = strrchr(path, '/');
  return slash ? slash + 1 : path;
}

bool isPhoto(const String& path, bool isDirectory) {
  return !isDirectory && photo_policy::isPhoto(basename(path.c_str()));
}

int comparePhotos(const void* left, const void* right) {
  const auto& a = *static_cast<const PhotoFile*>(left);
  const auto& b = *static_cast<const PhotoFile*>(right);
  return photo_policy::newestFirst(a.modified, a.path, b.modified, b.path);
}
}  // namespace

void PhotoCatalog::clear() {
  void* allocation = files_;
  core::memory::freePsram(allocation);
  files_ = nullptr;
  count_ = 0;
}

bool PhotoCatalog::removeAt(size_t index) {
  if (index >= count_ || !removeFile(files_[index].path)) return false;
  --count_;
  if (index < count_) {
    memmove(files_ + index, files_ + index + 1, (count_ - index) * sizeof(PhotoFile));
  }
  if (!count_) clear();
  return true;
}

PhotoCatalogResult PhotoCatalog::load() {
  clear();
  timings_ = {};
  const uint32_t started = micros();
  if (!mounted()) return PhotoCatalogResult::NoSd;
  {
    SpiLock lock;
    if (!SD.exists("/photos")) return PhotoCatalogResult::Ok;
    File dir = SD.open("/photos");
    if (!dir || !dir.isDirectory()) return PhotoCatalogResult::ReadError;

    // Enumerate directory names without opening/stat-ing every JPEG. Keep two
    // cheap passes so the PSRAM allocation is still exactly sized.
    size_t capacity = 0;
    bool isDirectory = false;
    for (String path = dir.getNextFileName(&isDirectory); path.length();
         path = dir.getNextFileName(&isDirectory)) {
      if (isPhoto(path, isDirectory)) ++capacity;
    }
    if (!capacity) return PhotoCatalogResult::Ok;
    if (capacity > SIZE_MAX / sizeof(PhotoFile)) return PhotoCatalogResult::NoMemory;
    const uint32_t allocationStarted = micros();
    files_ = static_cast<PhotoFile*>(core::memory::allocPsram(
        "photos.catalog", capacity * sizeof(PhotoFile)));
    timings_.allocationUs = micros() - allocationStarted;
    if (!files_) return PhotoCatalogResult::NoMemory;

    dir.rewindDirectory();
    for (String path = dir.getNextFileName(&isDirectory); path.length();
         path = dir.getNextFileName(&isDirectory)) {
      if (isPhoto(path, isDirectory)) {
        if (count_ >= capacity) {
          clear();
          return PhotoCatalogResult::ReadError;
        }
        PhotoFile& photo = files_[count_];
        const int length = snprintf(photo.path, sizeof(photo.path), "/photos/%s", basename(path.c_str()));
        if (length < 0 || (size_t)length >= sizeof(photo.path)) {
          clear();
          return PhotoCatalogResult::ReadError;
        }
        photo.modified = 0;
        ++count_;
      }
    }
    if (count_ != capacity) {
      clear();
      return PhotoCatalogResult::ReadError;
    }
    // A unique camera ID determines its order regardless of timestamp. Read
    // timestamps only for imports or duplicate IDs, preserving the old tie order.
    qsort(files_, count_, sizeof(PhotoFile), comparePhotos);
    for (size_t first = 0; first < count_;) {
      const uint64_t sequence = photo_policy::sequence(files_[first].path);
      size_t end = first + 1;
      while (end < count_ && photo_policy::sequence(files_[end].path) == sequence) ++end;
      if (!sequence || end - first > 1) {
        for (size_t index = first; index < end; ++index) {
          File entry = SD.open(files_[index].path, FILE_READ);
          if (!entry) { clear(); return PhotoCatalogResult::ReadError; }
          const time_t modified = entry.getLastWrite();
          files_[index].modified = modified > 0 ? (uint64_t)modified : 0;
          ++timings_.metadataReads;
        }
      }
      first = end;
    }
  }
  qsort(files_, count_, sizeof(PhotoFile), comparePhotos);
  timings_.totalUs = micros() - started;
  return PhotoCatalogResult::Ok;
}

}  // namespace sd
}  // namespace hal
